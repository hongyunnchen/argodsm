/**
 * @file
 * @brief This file provides a cohort lock for the ArgoDSM system
 * @todo Better documentation
 * @copyright Eta Scale AB. Licensed under the ArgoDSM Open Source License. See the LICENSE file for details.
 */

#ifndef argo_cohort_lock_hpp
#define argo_cohort_lock_hpp argo_cohort_lock_hpp

#include "../allocators/collective_allocator.hpp"
#include "../backend/backend.hpp"
#include "../data_distribution/data_distribution.hpp"
#include "global_tas_lock.hpp"
#include "intranode/mcs_lock.hpp"
#include "intranode/ticket_lock.hpp"

#include <vector>

#include <sched.h>
#include <numa.h>

extern "C" {
#include "cohort_lock.h"
}

namespace argo {
	namespace globallock {
		/**
		 * @brief a global  'cohort' lock - needs to be called collectively
		 * @details Locks in levels and tries to hand over the lock as locally
		 *         as possible.
		 * @warning Do not allocate this lock on the global memory. It contains
		 *         data that need to be node local. Also, keep in mind that the
		 *         constructor calls conew_, so the constructor should be called
		 *         by all the nodes at the same time.
		 *
		 * @todo Allow for global and dynamic allocation
		 */
		class cohort_lock {
			private:
				/** @brief To keep track if the local ArgoDSM node has the global lock or not */
				bool has_global_lock;

				/** @brief Keeps track of how many times we handed over the NUMA lock on a specific NUMA node */
				int numanodes;

				/** @brief keeps track of which NUMA node has the NUMA lock */
				int *handovers;

				/** @brief Keeps track of how many times we handed over the global lock between the NUMA nodes */
				int numahandover;

				/** @brief number of NUMA nodes in the system */
				std::atomic<int> nodelockowner;

				/** @brief which node the lock is/was locked in */
				int node;

				/** @brief Mapping between CPUs and NUMA nodes */
				std::vector<int> numa_mapping;

				/** @brief Flag necessary for the global_lock */
				bool *tas_flag;

				/** @brief A global TAS lock for locking between ArgoDSM nodes */
				argo::globallock::global_tas_lock *global_lock;

				/** @brief Local MCS locks for locking internally on a NUMA node */
				argo::locallock::mcs_lock *local_lock;

				/** @brief A local MCS lock shared by all the NUMA nodes - used for handing lock over between HW NUMA nodes */
				argo::locallock::ticket_lock *node_lock;

				/** @brief maximum amount of local handovers withing a NUMA node - numbers are experimental */
				static const int MAX_HANDOVER=8192;

				/** @brief maximum amount of local handovers between NUMA nodes on the same ArgoDSM node - numbers are experimental */
				static const int MAX_HANDOVER_NODELOCK=128;

				/** @brief Constant for no NUMA node having the node_lock */
				static const int NO_OWNER = -1;

				/**
				 * @brief Return the NUMA node in which the calling thread is being run.
				 * @return The NUMA node ID
				 *
				 * This function utilizes the sched_getcpu and the
				 * numa_node_of_cpu function. While the first is very quick, the
				 * second is extremely slow, so the results are preinitialized
				 * at the constructor.
				 */
				int numa_node() {
					/*
					 * int cpu = sched_getcpu();
					 * return numa_mapping[cpu];
					 */
					return 0; /// @bug FIXME
				}

			public:
				/**
				 * @brief construct global 'cohort' lock.
				 *
				 * This lock performs handovers in three levels: First within
				 * the same NUMA node, then within the same ArgoDSM node, and
				 * finally over ArgoDSM nodes.
				 */
				cohort_lock() :
					has_global_lock(false),
					numanodes(numa_num_configured_nodes()),
					handovers(new int[numanodes]()),
					numahandover(0),
					nodelockowner(NO_OWNER),
					tas_flag(argo::conew_<bool>(false)),
					global_lock(new argo::globallock::global_tas_lock(tas_flag)),
					local_lock(new argo::locallock::mcs_lock[numanodes]),
					node_lock(new argo::locallock::ticket_lock())
				{
					// Initialize the NUMA map
					int num_cpus = numa_num_configured_cpus();
					numa_mapping.resize(num_cpus);
					for (int i = 0; i < num_cpus; ++i)
						numa_mapping[i] = numa_node_of_cpu(i);
				}

				/** @todo Documentation */
				~cohort_lock(){
					codelete_(tas_flag);
					delete global_lock;
					delete[] local_lock;
					delete node_lock;
					delete[] handovers;
				}

				/**
				 * @brief Release the lock
				 */
				void unlock() {
					/* Check if we can hand over the lock locally */
					if(local_lock[node].is_contended() && handovers[node] < MAX_HANDOVER){
						handovers[node]++;
					}
					else{
						/* Cant hand over locally in the NUMA node - releases the NUMA lock */
						handovers[node] = 0;
						nodelockowner = NO_OWNER;

						/* check if we should hand over to another NUMA node or ArgoDSM node */
						if(node_lock->is_contended() && numahandover < MAX_HANDOVER_NODELOCK){
							/* Hand over to another NUMA node */
							numahandover++;
						}
						else{
							/* hand over to another ArgoDSM node */
							has_global_lock = false;
							numahandover = 0;
							global_lock->unlock();
						}
						node_lock->unlock();
					}
					local_lock[node].unlock();
				}

				/**
				 * @brief Acquire the lock
				 */
				void lock() {
					node = numa_node();

					/* Take the local lock for your NUMA node */
					local_lock[node].lock();
					/* Checks if this NUMA node already has the node_lock or not */
					if(node != nodelockowner){
						/* Take the node_lock and set that this NUMA node has the node_lock */
						node_lock->lock();
						nodelockowner = node;
						/* Check if this ArgoDSM node has the global lock or not */
						if(!has_global_lock){
							/* Take the global lock */
							global_lock->lock();
							has_global_lock = true;
						}
					}
				}
		};
	} // namespace globallock
} // namespace argo

#endif /* argo_cohort_lock_hpp */
