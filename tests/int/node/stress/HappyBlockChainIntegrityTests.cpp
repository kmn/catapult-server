/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "sdk/src/extensions/TransactionExtensions.h"
#include "catapult/chain/BlockScorer.h"
#include "catapult/config/ValidateConfiguration.h"
#include "catapult/model/ChainScore.h"
#include "tests/int/node/stress/test/BlockChainBuilder.h"
#include "tests/int/node/test/LocalNodeRequestTestUtils.h"
#include "tests/int/node/test/LocalNodeTestContext.h"
#include "tests/test/nodeps/Logging.h"
#include "tests/test/nodeps/MijinConstants.h"
#include "tests/TestHarness.h"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace catapult { namespace local {

#define TEST_CLASS HappyBlockChainIntegrityTests

	namespace {
		constexpr size_t Default_Network_Size = 10;
		constexpr uint32_t Max_Rollback_Blocks = 124;

		struct HappyLocalNodeTraits {
			static constexpr auto CountersToLocalNodeStats = test::CountersToBasicLocalNodeStats;
			static constexpr auto AddPluginExtensions = test::AddSimplePartnerPluginExtensions;
			static constexpr auto ShouldRegisterPreLoadHandler = false;
		};

		using NodeTestContext = test::LocalNodeTestContext<HappyLocalNodeTraits>;

		uint16_t GetPortForNode(uint32_t id) {
			return static_cast<uint16_t>(test::Local_Host_Port + 10 * (id + 1));
		}

		ionet::Node CreateNode(uint32_t id) {
			auto metadata = ionet::NodeMetadata(model::NetworkIdentifier::Mijin_Test, "NODE " + std::to_string(id));
			metadata.Roles = ionet::NodeRoles::Peer;
			return ionet::Node(
					crypto::KeyPair::FromString(test::Mijin_Test_Private_Keys[id]).publicKey(),
					test::CreateLocalHostNodeEndpoint(GetPortForNode(id)),
					metadata);
		}

		std::vector<ionet::Node> CreateNodes(size_t numNodes) {
			std::vector<ionet::Node> nodes;
			for (auto i = 0u; i < numNodes; ++i)
				nodes.push_back(CreateNode(i));

			return nodes;
		}

		void UpdateBlockChainConfiguration(model::BlockChainConfiguration& blockChainConfig) {
			blockChainConfig.ImportanceGrouping = Max_Rollback_Blocks / 2 + 1;
			blockChainConfig.MaxRollbackBlocks = Max_Rollback_Blocks;
			blockChainConfig.MaxDifficultyBlocks = Max_Rollback_Blocks - 1;
		}

		void UpdateConfigurationForNode(config::LocalNodeConfiguration& config, uint32_t id) {
			// 1. give each node its own ports
			auto port = GetPortForNode(id);
			auto& nodeConfig = const_cast<config::NodeConfiguration&>(config.Node);
			nodeConfig.Port = port;
			nodeConfig.ApiPort = port + 1;

			// 2. specify custom network settings
			UpdateBlockChainConfiguration(const_cast<model::BlockChainConfiguration&>(config.BlockChain));

			// 3. give each node its own key
			auto& userConfig = const_cast<config::UserConfiguration&>(config.User);
			userConfig.BootKey = test::Mijin_Test_Private_Keys[id];

			// 4. ensure configuration is valid
			ValidateConfiguration(config);
		}

		void RescheduleTasks(const std::string resourcesDirectory) {
			namespace pt = boost::property_tree;

			auto configFilePath = (boost::filesystem::path(resourcesDirectory) / "config-task.properties").generic_string();

			pt::ptree properties;
			pt::read_ini(configFilePath, properties);

			// 1. reconnect more rapidly so nodes have a better chance to find each other
			properties.put("connect peers task for service Sync.startDelay", "2s");
			properties.put("connect peers task for service Sync.repeatDelay", "500ms");

			// 2. run far more frequent sync rounds but delay initial sync to allow all nodes to receive their initial chains via push
			properties.put("synchronizer task.startDelay", "5s");
			properties.put("synchronizer task.repeatDelay", "500ms");

			pt::write_ini(configFilePath, properties);
		}

		uint8_t RandomByteClamped(uint8_t max) {
			return test::RandomByte() * max / std::numeric_limits<uint8_t>::max();
		}

		struct ChainStatistics {
			model::ChainScore Score;
			Hash256 StateHash;
			catapult::Height Height;
		};

		ChainStatistics PushRandomBlockChainToNode(
				const ionet::Node& node,
				test::StateHashCalculator&& stateHashCalculator,
				size_t numBlocks,
				Timestamp blockTimeInterval) {
			constexpr uint32_t Num_Accounts = 11;
			test::Accounts accounts(Num_Accounts);

			auto blockChainConfig = test::CreateLocalNodeBlockChainConfiguration();
			UpdateBlockChainConfiguration(blockChainConfig);

			test::BlockChainBuilder builder(accounts, stateHashCalculator, blockChainConfig);
			builder.setBlockTimeInterval(blockTimeInterval);

			for (auto i = 0u; i < numBlocks; ++i) {
				// don't allow account 0 to be recipient because it is sender
				auto recipientId = RandomByteClamped(Num_Accounts - 2) + 1u;
				builder.addTransfer(0, recipientId, Amount(1'000'000));
			}

			auto blocks = builder.asBlockChain();

			test::ExternalSourceConnection connection(node);
			test::PushEntities(connection, ionet::PacketType::Push_Block, blocks);

			const auto& lastBlock = *blocks.back();
			ChainStatistics chainStats;
			chainStats.StateHash = lastBlock.StateHash;
			chainStats.Height = lastBlock.Height;

			mocks::MockMemoryBlockStorage storage;
			auto pNemesisBlockElement = storage.loadBlockElement(Height(1));
			chainStats.Score = model::ChainScore(chain::CalculateScore(pNemesisBlockElement->Block, *blocks[0]));
			for (auto i = 0u; i < blocks.size() - 1; ++i)
				chainStats.Score += model::ChainScore(chain::CalculateScore(*blocks[i], *blocks[i + 1]));

			return chainStats;
		}

		struct HappyLocalNodeStatistics : public ChainStatistics {
			uint64_t NumActiveReaders;
			uint64_t NumActiveWriters;
		};

		HappyLocalNodeStatistics GetStatistics(const NodeTestContext& context) {
			const auto& localNodeStats = context.stats();
			const auto& cacheView = context.localNode().cache().createView();

			HappyLocalNodeStatistics stats;
			stats.Score = context.localNode().score();
			stats.StateHash = cacheView.calculateStateHash().StateHash;
			stats.Height = cacheView.height();
			stats.NumActiveReaders = localNodeStats.NumActiveReaders;
			stats.NumActiveWriters = localNodeStats.NumActiveWriters;
			return stats;
		}

		void LogStatistics(const ionet::Node& node, const ChainStatistics& stats) {
			CATAPULT_LOG(debug)
					<< "*** CHAIN STATISTICS FOR NODE: " << node << " ***" << std::endl
					<< " ------ score " << stats.Score << std::endl
					<< " - state hash " << utils::HexFormat(stats.StateHash) << std::endl
					<< " ----- height " << stats.Height;
		}

		void LogStatistics(const ionet::Node& node, const HappyLocalNodeStatistics& stats) {
			CATAPULT_LOG(debug)
					<< "*** STATISTICS FOR NODE: " << node << " ***" << std::endl
					<< " ------ score " << stats.Score << std::endl
					<< " - state hash " << utils::HexFormat(stats.StateHash) << std::endl
					<< " ----- height " << stats.Height << std::endl
					<< " ---- readers " << stats.NumActiveReaders << std::endl
					<< " ---- writers " << stats.NumActiveWriters;
		}

		// region network traits

		struct DenseNetworkTraits {
			static std::vector<ionet::Node> GetPeersForNode(uint32_t, const std::vector<ionet::Node>& networkNodes) {
				return networkNodes;
			}
		};

		struct SparseNetworkTraits {
			static std::vector<ionet::Node> GetPeersForNode(uint32_t id, const std::vector<ionet::Node>& networkNodes) {
				// let each node only pull from "next" node
				return { networkNodes[(id + 1) % (networkNodes.size() - 1)] };
			}
		};

		// endregion

		// region state hash traits

		class StateHashDisabledTraits {
		public:
			static constexpr auto Node_Flag = test::NodeFlag::Regular;

		public:
			test::StateHashCalculator createStateHashCalculator(const NodeTestContext&) const {
				return test::StateHashCalculator();
			}
		};

		class StateHashEnabledTraits {
		public:
			static constexpr auto Node_Flag = test::NodeFlag::Verify_State;

		public:
			StateHashEnabledTraits()
					: m_stateHashCalculationDir("../temp/statehash") // isolated directory used for state hash calculation
			{}

		public:
			test::StateHashCalculator createStateHashCalculator(const NodeTestContext& context) const {
				{
					test::TempDirectoryGuard forceCleanResourcesDir(m_stateHashCalculationDir.name());
				}

				return test::StateHashCalculator(context.prepareFreshDataDirectory(m_stateHashCalculationDir.name()));
			}

		private:
			test::TempDirectoryGuard m_stateHashCalculationDir;
		};

		// endregion

		template<typename TNetworkTraits, typename TStateHashTraits>
		void AssertMultiNodeNetworkCanReachConsensus(TStateHashTraits&& stateHashTraits, size_t networkSize) {
			// Arrange: create nodes
			test::GlobalLogFilter testLogFilter(utils::LogLevel::Info);
			auto networkNodes = CreateNodes(networkSize);

			// Act: boot all nodes
			std::vector<std::unique_ptr<NodeTestContext>> contexts;
			std::vector<uint64_t> chainHeights;
			ChainStatistics bestChainStats;
			for (auto i = 0u; i < networkSize; ++i) {
				// - give each node a separate directory
				auto nodeFlag = test::NodeFlag::Require_Explicit_Boot | TStateHashTraits::Node_Flag;
				auto peers = TNetworkTraits::GetPeersForNode(i, networkNodes);
				auto configTransform = [i](auto& config) {
					UpdateConfigurationForNode(config, i);
					const_cast<config::NodeConfiguration&>(config.Node).OutgoingConnections.MaxConnections = 20;
				};
				auto postfix = "_" + std::to_string(i);
				contexts.push_back(std::make_unique<NodeTestContext>(nodeFlag, peers, configTransform, postfix));

				// - (re)schedule a few tasks and boot the node
				auto& context = *contexts.back();
				RescheduleTasks(context.resourcesDirectory());
				context.boot();

				// - push a random number of different (valid) blocks to each node
				// - vary time spacing so that all chains will have different scores
				auto numBlocks = RandomByteClamped(Max_Rollback_Blocks - 1) + 1u; // always generate at least one block
				chainHeights.push_back(numBlocks + 1);
				auto chainStats = PushRandomBlockChainToNode(
						networkNodes[i],
						stateHashTraits.createStateHashCalculator(context),
						numBlocks,
						Timestamp(60'000 + i * 1'000));

				LogStatistics(networkNodes[i], chainStats);
				if (chainStats.Score > bestChainStats.Score)
					bestChainStats = chainStats;
			}

			// Assert: wait for nodes to sync among themselves
			for (auto i = 0u; i < networkSize; ++i) {
				const auto& node = networkNodes[i];
				const auto& context = *contexts[i];

				CATAPULT_LOG(debug) << "waiting for node " << node << " to get best chain (score = " << bestChainStats.Score << ")";
				LogStatistics(node, GetStatistics(context));

				try {
					// - block chain sync consumer updates score and then cache, so need to wait for both to avoid race condition
					WAIT_FOR_VALUE_EXPR_SECONDS(bestChainStats.Score, context.localNode().score(), 30);
					WAIT_FOR_VALUE_EXPR_SECONDS(bestChainStats.Height, context.localNode().cache().createView().height(), 10);

					const auto& stats = GetStatistics(context);
					LogStatistics(node, stats);

					// - nodes have shared state
					EXPECT_EQ(bestChainStats.Score, stats.Score);
					EXPECT_EQ(bestChainStats.StateHash, stats.StateHash);
					EXPECT_EQ(bestChainStats.Height, stats.Height);
				} catch (const catapult_runtime_error&) {
					// - log bit more information on failure
					LogStatistics(node, GetStatistics(context));
					throw;
				}
			}
		}
	}

	NO_STRESS_TEST(TEST_CLASS, MultiNodeDenseNetworkCanReachConsensus) {
		// Arrange: allow test to pass with low default MacOS file descriptor limit
#if defined __APPLE__
		static constexpr size_t Network_Size = 8;
#else
		static constexpr size_t Network_Size = Default_Network_Size;
#endif

		// Assert:
		AssertMultiNodeNetworkCanReachConsensus<DenseNetworkTraits>(StateHashDisabledTraits(), Network_Size);
	}

	NO_STRESS_TEST(TEST_CLASS, MultiNodeDenseNetworkCanReachConsensusWithStateHashEnabled) {
		// Arrange: allow test to pass with low default MacOS file descriptor limit
#if defined __APPLE__
		static constexpr size_t Network_Size = 4;
#else
		static constexpr size_t Network_Size = Default_Network_Size;
#endif

		// Assert:
		AssertMultiNodeNetworkCanReachConsensus<DenseNetworkTraits>(StateHashEnabledTraits(), Network_Size);
	}

	NO_STRESS_TEST(TEST_CLASS, MultiNodeSparseNetworkCanReachConsensus) {
		// Assert:
		AssertMultiNodeNetworkCanReachConsensus<SparseNetworkTraits>(StateHashDisabledTraits(), Default_Network_Size);
	}

	NO_STRESS_TEST(TEST_CLASS, MultiNodeSparseNetworkCanReachConsensusWithStateHashEnabled) {
		// Arrange: allow test to pass with low default MacOS file descriptor limit
#if defined __APPLE__
		static constexpr size_t Network_Size = 6;
#else
		static constexpr size_t Network_Size = Default_Network_Size;
#endif

		// Assert:
		AssertMultiNodeNetworkCanReachConsensus<SparseNetworkTraits>(StateHashEnabledTraits(), Network_Size);
	}
}}
