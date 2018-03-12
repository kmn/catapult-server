#include "catapult/extensions/ServerHooks.h"
#include "tests/test/cache/UtTestUtils.h"
#include "tests/test/core/BlockTestUtils.h"
#include "tests/test/core/TransactionInfoTestUtils.h"
#include "tests/test/other/ConsumerHandlerTests.h"
#include "tests/TestHarness.h"

namespace catapult { namespace extensions {

#define TEST_CLASS ServerHooksTests

	// region sinks + handlers

	namespace {
		struct NewBlockSinkTraits {
			static auto CreateConsumer(const ServerHooks& hooks) {
				return hooks.newBlockSink();
			}

			static void AddConsumer(ServerHooks& hooks, const NewBlockSink& sink) {
				hooks.addNewBlockSink(sink);
			}

			static auto CreateConsumerData() {
				return std::shared_ptr<const model::Block>(test::GenerateEmptyRandomBlock());
			}
		};

		struct NewTransactionsSinkTraits {
			static auto CreateConsumer(const ServerHooks& hooks) {
				return hooks.newTransactionsSink();
			}

			static void AddConsumer(ServerHooks& hooks, const SharedNewTransactionsSink& sink) {
				hooks.addNewTransactionsSink(sink);
			}

			static auto CreateConsumerData() {
				return consumers::TransactionInfos();
			}
		};

		struct PacketPayloadSinkTraits {
			static auto CreateConsumer(const ServerHooks& hooks) {
				return hooks.packetPayloadSink();
			}

			static void AddConsumer(ServerHooks& hooks, const PacketPayloadSink& sink) {
				hooks.addPacketPayloadSink(sink);
			}

			static auto CreateConsumerData() {
				return ionet::PacketPayload();
			}
		};

		struct TransactionsChangeHandlerTraits {
			static auto CreateConsumer(const ServerHooks& hooks) {
				return hooks.transactionsChangeHandler();
			}

			static void AddConsumer(ServerHooks& hooks, const TransactionsChangeHandler& handler) {
				hooks.addTransactionsChangeHandler(handler);
			}

			static auto CreateConsumerData() {
				// dangling references are ok because the struct fields are not accessed
				return consumers::TransactionsChangeInfo({}, {});
			}
		};

		struct TransactionEventHandlerTraits {
			static auto CreateConsumer(const ServerHooks& hooks) {
				return hooks.transactionEventHandler();
			}

			static void AddConsumer(ServerHooks& hooks, const TransactionEventHandler& handler) {
				hooks.addTransactionEventHandler(handler);
			}

			static auto CreateConsumerData() {
				// dangling references are ok because the struct fields are not accessed
				return TransactionEventData({}, TransactionEvent::Dependency_Removed);
			}
		};
	}

	DEFINE_CONSUMER_HANDLER_TESTS(TEST_CLASS, ServerHooks, NewBlockSink);
	DEFINE_CONSUMER_HANDLER_TESTS(TEST_CLASS, ServerHooks, NewTransactionsSink);
	DEFINE_CONSUMER_HANDLER_TESTS(TEST_CLASS, ServerHooks, PacketPayloadSink);
	DEFINE_CONSUMER_HANDLER_TESTS(TEST_CLASS, ServerHooks, TransactionsChangeHandler);
	DEFINE_CONSUMER_HANDLER_TESTS(TEST_CLASS, ServerHooks, TransactionEventHandler);

	// endregion

	// region consumer factories + retrievers

	namespace {
		struct BlockRangeConsumerFactoryTraits {
			static constexpr auto Input = disruptor::InputSource::Local; // input source passed to consumer factory

			static auto Get(const ServerHooks& hooks) {
				return hooks.blockRangeConsumerFactory();
			}

			static void Set(ServerHooks& hooks, const BlockRangeConsumerFactoryFunc& factory) {
				hooks.setBlockRangeConsumerFactory(factory);
			}

			static auto CreateResult() {
				return BlockRangeConsumerFunc();
			}
		};

		struct CompletionAwareBlockRangeConsumerFactoryTraits {
			static constexpr auto Input = disruptor::InputSource::Local; // input source passed to consumer factory

			static auto Get(const ServerHooks& hooks) {
				return hooks.completionAwareBlockRangeConsumerFactory();
			}

			static void Set(ServerHooks& hooks, const CompletionAwareBlockRangeConsumerFactoryFunc& factory) {
				hooks.setCompletionAwareBlockRangeConsumerFactory(factory);
			}

			static auto CreateResult() {
				return chain::CompletionAwareBlockRangeConsumerFunc();
			}
		};

		struct TransactionRangeConsumerFactoryTraits {
			static constexpr auto Input = disruptor::InputSource::Local; // input source passed to consumer factory

			static auto Get(const ServerHooks& hooks) {
				return hooks.transactionRangeConsumerFactory();
			}

			static void Set(ServerHooks& hooks, const TransactionRangeConsumerFactoryFunc& factory) {
				hooks.setTransactionRangeConsumerFactory(factory);
			}

			static auto CreateResult() {
				return TransactionRangeConsumerFunc();
			}
		};

		struct RemoteChainHeightsRetrieverTraits {
			static constexpr size_t Input = 123; // number of peers passed to retriever

			static auto Get(const ServerHooks& hooks) {
				return hooks.remoteChainHeightsRetriever();
			}

			static void Set(ServerHooks& hooks, const RemoteChainHeightsRetriever& retriever) {
				hooks.setRemoteChainHeightsRetriever(retriever);
			}

			static auto CreateResult() {
				return thread::make_ready_future(std::vector<Height>());
			}
		};
	}

#define FACTORY_RETRIEVER_TEST_ENTRY(TEST_NAME, CONSUMER_FACTORY_NAME) \
	TEST(TEST_CLASS, TEST_NAME##_##CONSUMER_FACTORY_NAME) { TRAITS_TEST_NAME(TEST_CLASS, TEST_NAME)<CONSUMER_FACTORY_NAME##Traits>(); }

#define FACTORY_RETRIEVER_TEST(TEST_NAME) \
	template<typename TTraits> void TRAITS_TEST_NAME(TEST_CLASS, TEST_NAME)(); \
	FACTORY_RETRIEVER_TEST_ENTRY(TEST_NAME, BlockRangeConsumerFactory) \
	FACTORY_RETRIEVER_TEST_ENTRY(TEST_NAME, CompletionAwareBlockRangeConsumerFactory) \
	FACTORY_RETRIEVER_TEST_ENTRY(TEST_NAME, TransactionRangeConsumerFactory) \
	FACTORY_RETRIEVER_TEST_ENTRY(TEST_NAME, RemoteChainHeightsRetriever) \
	template<typename TTraits> void TRAITS_TEST_NAME(TEST_CLASS, TEST_NAME)()

	FACTORY_RETRIEVER_TEST(CannotAccessWhenUnset) {
		// Arrange:
		ServerHooks hooks;

		// Act + Assert:
		EXPECT_THROW(TTraits::Get(hooks), catapult_invalid_argument);
	}

	FACTORY_RETRIEVER_TEST(CanSetOnce) {
		// Arrange:
		ServerHooks hooks;
		std::vector<typename std::remove_const<decltype(TTraits::Input)>::type> inputs;
		TTraits::Set(hooks, [&inputs](auto input) {
			inputs.push_back(input);
			return TTraits::CreateResult();
		});

		// Act:
		auto factory = TTraits::Get(hooks);
		ASSERT_TRUE(!!factory);

		factory(TTraits::Input);

		// Assert:
		ASSERT_EQ(1u, inputs.size());
		EXPECT_EQ(static_cast<typename decltype(inputs)::value_type>(TTraits::Input), inputs[0]);
	}

	FACTORY_RETRIEVER_TEST(CannotSetMultipleTimes) {
		// Arrange:
		ServerHooks hooks;
		TTraits::Set(hooks, [](auto) { return TTraits::CreateResult(); });

		// Act + Assert:
		EXPECT_THROW(TTraits::Set(hooks, [](auto) { return TTraits::CreateResult(); }), catapult_invalid_argument);
	}

	// endregion

	// region chainSyncedPredicate

	TEST(TEST_CLASS, UnsetChainSyncedPredicateReturnsTrue) {
		// Arrange:
		ServerHooks hooks;

		// Act:
		auto predicate = hooks.chainSyncedPredicate();
		ASSERT_TRUE(!!predicate);

		auto isSynced = predicate();

		// Assert:
		EXPECT_TRUE(isSynced);
	}

	TEST(TEST_CLASS, CanSetOnce_ChainSyncedPredicate) {
		// Arrange:
		auto numCalls = 0u;
		ServerHooks hooks;
		hooks.setChainSyncedPredicate([&numCalls]() {
			++numCalls;
			return false;
		});

		// Act:
		auto predicate = hooks.chainSyncedPredicate();
		ASSERT_TRUE(!!predicate);

		auto isSynced = predicate();

		// Assert:
		EXPECT_EQ(1u, numCalls);
		EXPECT_FALSE(isSynced);
	}

	TEST(TEST_CLASS, CannotSetMultipleTimes_ChainSyncedPredicate) {
		// Arrange:
		ServerHooks hooks;
		hooks.setChainSyncedPredicate([]() { return false; });

		// Act + Assert:
		EXPECT_THROW(hooks.setChainSyncedPredicate([]() { return false; }), catapult_invalid_argument);
	}

	// endregion

	// region knownHashPredicate

	namespace {
		constexpr auto Num_Infos_Per_Group = 5u;

		class KnownHashPredicateTestContext {
		public:
			KnownHashPredicateTestContext()
					: m_utCache(cache::MemoryCacheOptions(Num_Infos_Per_Group, Num_Infos_Per_Group))
					, m_transactionInfos(test::CreateTransactionInfos(Num_Infos_Per_Group)) {
				test::AddAll(m_utCache, m_transactionInfos);
			}

		public:
			void addKnownHashPredicate(const std::vector<model::TransactionInfo>& transactionInfos) {
				m_hooks.addKnownHashPredicate([&transactionInfos](auto timestamp, const auto& hash) {
					return std::any_of(
							transactionInfos.cbegin(),
							transactionInfos.cend(),
							[timestamp, &hash](const auto& transactionInfo) {
								return timestamp == transactionInfo.pEntity->Deadline && hash == transactionInfo.EntityHash;
							});
				});
			}

			void createPredicate() {
				m_predicate = m_hooks.knownHashPredicate(m_utCache);
				ASSERT_TRUE(!!m_predicate);
			}

		public:
			void assertBasicPredicateResults() {
				// Assert: all infos in utCache should be known
				assertAllAreKnown(m_transactionInfos);

				// - random infos should be unknown
				assertNoneAreKnown(test::CreateTransactionInfos(Num_Infos_Per_Group));
			}

		public:
			void assertAllAreKnown(const std::vector<model::TransactionInfo>& transactionInfos) {
				for (const auto& transactionInfo : transactionInfos)
					EXPECT_TRUE(m_predicate(transactionInfo.pEntity->Deadline, transactionInfo.EntityHash));
			}

			void assertNoneAreKnown(const std::vector<model::TransactionInfo>& transactionInfos) {
				for (const auto& transactionInfo : transactionInfos)
					EXPECT_FALSE(m_predicate(transactionInfo.pEntity->Deadline, transactionInfo.EntityHash));
			}

		private:
			ServerHooks m_hooks;
			cache::MemoryUtCache m_utCache;
			std::vector<model::TransactionInfo> m_transactionInfos;
			KnownHashPredicate m_predicate;
		};
	}

	TEST(TEST_CLASS, UnsetKnownHashPredicateDelegatesToUtCache) {
		// Arrange:
		KnownHashPredicateTestContext context;

		// Act:
		context.createPredicate();

		// Assert:
		context.assertBasicPredicateResults();
	}

	TEST(TEST_CLASS, SetKnownHashPredicateDelegatesToUtCacheAndPredicate) {
		// Arrange:
		KnownHashPredicateTestContext context;

		auto transactionInfos = test::CreateTransactionInfos(Num_Infos_Per_Group);
		context.addKnownHashPredicate(transactionInfos);

		// Act:
		context.createPredicate();

		// Assert:
		context.assertBasicPredicateResults();

		// - all infos known by predicate should be known
		context.assertAllAreKnown(transactionInfos);
	}

	TEST(TEST_CLASS, CanAddMultipleKnownHashPredicates) {
		// Arrange:
		KnownHashPredicateTestContext context;

		auto transactionInfos = test::CreateTransactionInfos(Num_Infos_Per_Group);
		context.addKnownHashPredicate(transactionInfos);

		auto transactionInfos2 = test::CreateTransactionInfos(Num_Infos_Per_Group);
		context.addKnownHashPredicate(transactionInfos2);

		// Act:
		context.createPredicate();

		// Assert:
		context.assertBasicPredicateResults();

		// - all infos known by either predicate should be known
		context.assertAllAreKnown(transactionInfos);
		context.assertAllAreKnown(transactionInfos2);
	}

	// endregion
}}