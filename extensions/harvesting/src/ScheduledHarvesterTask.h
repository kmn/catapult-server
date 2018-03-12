#pragma once
#include "Harvester.h"
#include "catapult/chain/ChainFunctions.h"
#include "catapult/disruptor/DisruptorTypes.h"
#include "catapult/model/Elements.h"
#include "catapult/model/EntityInfo.h"
#include "catapult/model/RangeTypes.h"
#include "catapult/functions.h"

namespace catapult { namespace harvesting {

	/// Options for the harvesting task.
	struct ScheduledHarvesterTaskOptions {
		/// Indicates if harvesting is allowed.
		predicate<> HarvestingAllowed;

		/// Supplies information about the last block of the chain.
		supplier<std::shared_ptr<const model::BlockElement>> LastBlockElementSupplier;

		/// Supplies the current network time.
		chain::TimeSupplier TimeSupplier;

		/// Consumes a range consisting of the harvested block, usually delivers it to the disruptor queue.
		consumer<model::BlockRange&&, const disruptor::ProcessingCompleteFunc&> RangeConsumer;
	};

	/// Class that lets a harvester create a block and supplies the block to a consumer.
	class ScheduledHarvesterTask {
	public:
		using TaskOptions = ScheduledHarvesterTaskOptions;

	public:
		/// Creates a scheduled harvesting task around \a options and a \a pHarvester.
		explicit ScheduledHarvesterTask(const ScheduledHarvesterTaskOptions& options, std::unique_ptr<Harvester>&& pHarvester)
				: m_harvestingAllowed(options.HarvestingAllowed)
				, m_lastBlockElementSupplier(options.LastBlockElementSupplier)
				, m_timeSupplier(options.TimeSupplier)
				, m_rangeConsumer(options.RangeConsumer)
				, m_pHarvester(std::move(pHarvester))
				, m_isAnyHarvestedBlockPending(false)
		{}

	public:
		/// Triggers the harvesting process and in case of successfull block creation
		/// supplies the block to the consumer.
		void harvest();

	private:
		const decltype(TaskOptions::HarvestingAllowed) m_harvestingAllowed;
		const decltype(TaskOptions::LastBlockElementSupplier) m_lastBlockElementSupplier;
		const decltype(TaskOptions::TimeSupplier) m_timeSupplier;
		const decltype(TaskOptions::RangeConsumer) m_rangeConsumer;
		std::unique_ptr<Harvester> m_pHarvester;

		std::atomic_bool m_isAnyHarvestedBlockPending;
	};
}}