/**
 * @file
 * @brief This file implements the MPI-backend of ArgoDSM
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */
#include "swdsm.h"

/*Treads*/
/** @brief Thread loads data into cache */
pthread_t loadthread1;
/** @brief Thread loads data into cache with an overlapping request (some parts are done in parallel) */
pthread_t loadthread2;
/** @brief Thread writes data remotely if parts of writebuffer */
pthread_t writethread;
/** @brief For matching threads to more sensible thread IDs */
pthread_t tid[NUM_THREADS] = {0};

/*Barrier*/
/** @brief  Locks access to part that does SD in the global barrier */
pthread_mutex_t barriermutex = PTHREAD_MUTEX_INITIALIZER;
/** @brief Thread local barrier used to first wait for all local threads in the global barrier*/
pthread_barrier_t *threadbarrier;


/*Pagecache*/
/** @brief  Size of the cache in number of pages*/
unsigned long cachesize;
/** @brief  Offset off the cache in the backing file*/
unsigned long cacheoffset;
/** @brief  Keeps state, tag and dirty bit of the cache*/
control_data * cacheControl;
/** @brief  keeps track of readers and writers*/
unsigned long *globalSharers;
/** @brief  size of pyxis directory*/
unsigned long classificationSize;
/** @brief  Tracks if a page is touched this epoch*/
argo_byte * touchedcache;
/** @brief  The local page cache*/
argo_byte * cacheData;
/** @brief Copy of the local cache to keep twinpages for later being able to DIFF stores */
char * pagecopy;
/** @brief Protects the pagecache */
pthread_mutex_t cachemutex = PTHREAD_MUTEX_INITIALIZER;

/*Writebuffer*/
/** @brief  Size of the writebuffer*/
size_t writebuffersize;
/** @brief  Writebuffer for storing indices to pages*/
unsigned long  *writebuffer;
/** @brief Most recent entry in the writebuffer*/
unsigned long  writebufferstart;
/** @brief Least recent entry in the writebuffer*/
unsigned long  writebufferend;
/** @brief Writethread wait on this to start a write from the writebuffer */
sem_t writerwaitsem;
/** @brief Writethread signals this when writing from writebuffer is done */
sem_t writerstartsem;
/** @brief Lock for the writebuffer*/
pthread_mutex_t wbmutex = PTHREAD_MUTEX_INITIALIZER;

/*MPI and Comm*/
/** @brief  A copy of MPI_COMM_WORLD group to split up processes into smaller groups*/
/** @todo This can be removed now when we are only running 1 process per ArgoDSM node */
MPI_Group startgroup;
/** @brief  A group of all processes that are executing the main thread */
/** @todo This can be removed now when we are only running 1 process per ArgoDSM node */
MPI_Group workgroup;
/** @brief Communicator can be replaced with MPI_COMM_WORLD*/
MPI_Comm workcomm;
/** @brief MPI window for communicating pyxis directory*/
MPI_Win sharerWindow;
/** @brief MPI window for communicating global locks*/
MPI_Win lockWindow;
/** @brief MPI windows for reading and writing data in global address space */
MPI_Win *globalDataWindow;
/** @brief MPI data structure for sending cache control data*/
MPI_Datatype mpi_control_data;
/** @brief MPI data structure for a block containing an ArgoDSM cacheline of pages */
MPI_Datatype cacheblock;
/** @brief number of MPI processes / ArgoDSM nodes */
int numtasks;
/** @brief  rank/process ID in the MPI/ArgoDSM runtime*/
int rank;
/** @brief rank/process ID in the MPI/ArgoDSM runtime*/
int workrank;
/** @brief tracking which windows are used for reading and writing global address space*/
char * barwindowsused;
/** @brief Semaphore protecting infiniband accesses*/
/** @todo replace with a (qd?)lock */
sem_t ibsem;

/*Loading and Prefetching*/
/** @brief Tracking address of pages that should be loaded by loadthread1 */
unsigned long *loadline;
/** @brief Tracking cacheindex of pages that should be loaded by loadthread1 */
unsigned long *loadtag;
/** @brief Tracking address of pages that should be loaded by loadthread2 */
unsigned long *prefetchline;
/** @brief Tracking cacheindex of pages that should be loaded by loadthread2 */
unsigned long *prefetchtag;
/** @brief loadthread1 waits on this to start loading remote pages */
sem_t loadstartsem;
/** @brief signalhandler waits on this to complete a transfer */
sem_t loadwaitsem;
/** @brief loadthread2 waits on this to start loading remote pages */
sem_t prefetchstartsem;
/** @brief signalhandler waits on this to complete a transfer */
sem_t prefetchwaitsem;

/*Backing file*/
/** @brief  Filename for file backing cache*/
char filename[MPI_MAX_PROCESSOR_NAME];
/** @brief  file descriptor for backing file*/
unsigned int fd;

/*Global lock*/
/** @brief  Local flags we spin on for the global lock*/
unsigned long * lockbuffer;
/** @brief  Protects the global lock so only 1 thread can have a global lock at a time */
sem_t globallocksem;
/** @brief  Keeps track of what local flag we should spin on per lock*/
int locknumber=0;

/*Global allocation*/
/** @brief  Keeps track of allocated memory in the global address space*/
unsigned long *allocationOffset;
/** @brief  Protects access to global allocator*/
pthread_mutex_t gmallocmutex = PTHREAD_MUTEX_INITIALIZER;

/*Common*/
/** @brief  Points to start of global address space*/
void * startAddr;
/** @brief  Points to start of global address space this process is serving */
argo_byte * globalData;
/** @brief  Size of global address space*/
unsigned long size_of_all;
/** @brief  Size of this process part of global address space*/
unsigned long size_of_chunk;
/** @brief  size of a page */
static const unsigned int pagesize = 4096;
/** @brief  Magic value for invalid cacheindices */
unsigned long GLOBAL_NULL;
/** @brief  Statistics */
argo_statistics stats;

unsigned long isPowerOf2(unsigned long x){
  unsigned long retval =  ((x & (x - 1)) == 0); //Checks if x is power of 2 (or zero)
  return retval;
}

void flushWriteBuffer(void){
	unsigned long i,j;
	double t1,t2;

	t1 = MPI_Wtime();
	pthread_mutex_lock(&wbmutex);

  for(i = 0; i < cachesize; i+=CACHELINE){
    unsigned long distrAddr = cacheControl[i].tag;
    if(distrAddr != GLOBAL_NULL){

      unsigned long distrAddr = cacheControl[i].tag;
      unsigned long lineAddr = distrAddr/(CACHELINE*pagesize);
      lineAddr*=(pagesize*CACHELINE);
			void * lineptr = (char*)startAddr + lineAddr;

      argo_byte dirty = cacheControl[i].dirty;
			if(dirty == DIRTY){
				mprotect(lineptr, pagesize*CACHELINE, PROT_READ);
				cacheControl[i].dirty=CLEAN;
				for(j=0; j < CACHELINE; j++){
					storepageDIFF(i+j,pagesize*j+lineAddr);
				}
			}
    }
  }

	for(i = 0; i < (unsigned long)numtasks; i++){
		if(barwindowsused[i] == 1){
			MPI_Win_unlock(i, globalDataWindow[i]);
			barwindowsused[i] = 0;
		}
	}

	writebufferstart = 0;
	writebufferend = 0;

	pthread_mutex_unlock(&wbmutex);
	t2 = MPI_Wtime();
	stats.flushtime += t2-t1;
}

void addToWriteBuffer(unsigned long cacheIndex){
	pthread_mutex_lock(&wbmutex);
	unsigned long line = cacheIndex/CACHELINE;
	line *= CACHELINE;

	if(writebuffer[writebufferend] == line ||
		 writebuffer[writebufferstart] == line){
		pthread_mutex_unlock(&wbmutex);
		return;
	}
  unsigned long wbendplusone = ((writebufferend+1)%writebuffersize);
	unsigned long wbendplustwo = ((writebufferend+2)%writebuffersize);
	if(wbendplusone == writebufferstart ){ // Buffer is full wait for slot to be empty
		double t1 = MPI_Wtime();
		sem_post(&writerstartsem);
		sem_wait(&writerwaitsem);
		double t4 = MPI_Wtime();
		stats.writebacks+=CACHELINE;
		stats.writebacktime+=(t4-t1);
		writebufferstart = wbendplustwo;
	}
	writebuffer[writebufferend] = line;
	writebufferend = wbendplusone;
	pthread_mutex_unlock(&wbmutex);
}


int argo_get_local_tid(){
	int i;
	for(i = 0; i < NUM_THREADS; i++){
		if(pthread_equal(tid[i],pthread_self())){
			return i;
		}
	}
	return 0;
}

int argo_get_global_tid(){
	int i;
	for(i = 0; i < NUM_THREADS; i++){
		if(pthread_equal(tid[i],pthread_self())){
			return ((getID()*NUM_THREADS) + i);
		}
	}
	return 0;
}


void argo_register_thread(){
	int i;
	sem_wait(&ibsem);
	for(i = 0; i < NUM_THREADS; i++){
		if(tid[i] == 0){
			tid[i] = pthread_self();
			break;
		}
	}
	sem_post(&ibsem);
	pthread_barrier_wait(&threadbarrier[NUM_THREADS]);
}


void argo_pin_threads(){

  cpu_set_t cpuset;
  int s;
  argo_register_thread();
  sem_wait(&ibsem);
  CPU_ZERO(&cpuset);
  int pinto = argo_get_local_tid();
  CPU_SET(pinto, &cpuset);

  s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (s != 0){
    printf("PINNING ERROR\n");
    argo_finalize();
  }
  sem_post(&ibsem);
}


//Get cacheindex
unsigned long getCacheIndex(unsigned long addr){
	unsigned long index = (addr/pagesize) % cachesize;
	return index;
}

void init_mpi_struct(void){
	//init our struct coherence unit to work in mpi.
	const int blocklen[3] = { 1,1,1};
	MPI_Aint offsets[3];
	offsets[0] = 0;  offsets[1] = sizeof(argo_byte)*1;  offsets[2] = sizeof(argo_byte)*2;

	MPI_Datatype types[3] = {MPI_BYTE,MPI_BYTE,MPI_UNSIGNED_LONG};
	MPI_Type_create_struct(3,blocklen, offsets, types, &mpi_control_data);

	MPI_Type_commit(&mpi_control_data);
}


void init_mpi_cacheblock(void){
	//init our struct coherence unit to work in mpi.
	MPI_Type_contiguous(pagesize*CACHELINE,MPI_BYTE,&cacheblock);
	MPI_Type_commit(&cacheblock);
}

unsigned long alignAddr(unsigned long addr){
	unsigned long mod = addr % pagesize;
	if(addr % pagesize != 0){
		addr = addr - mod;
	}
	addr /= pagesize;
	addr *= pagesize;
	return addr;
}

/**
 * @brief old signal handler
 * @details The last signal handler for SIGSEGV. This should only be set the first time
 *          we set our own signal handler.
 */
struct sigaction old_sigaction;

/**
 * @brief Restore the last stored signal handler for SIGSEGV
 */
static void restore_old_sigaction() {
	sigaction(SIGSEGV, &old_sigaction, NULL);
}

void handler(int sig, siginfo_t *si, void *unused){
	UNUSED_PARAM(sig);
	UNUSED_PARAM(unused);
	double t1 = MPI_Wtime();

	/* check whether address is out of bounds (lower end) of shared memory area */
	if (si->si_addr < startAddr) {
		restore_old_sigaction();
		return;
	}
	unsigned long tag;
	argo_byte owner,state;
	unsigned long distrAddr =  (unsigned long)((unsigned long)(si->si_addr) - (unsigned long)(startAddr));

	/* check whether address is out of bounds (upper end) of shared memory area */
	if (distrAddr >= size_of_all) {
		restore_old_sigaction();
		return;
	}
	unsigned long alignedDistrAddr = alignAddr(distrAddr);
	unsigned long remCACHELINE = alignedDistrAddr % (CACHELINE*pagesize);
	unsigned long lineAddr = alignedDistrAddr - remCACHELINE;
	unsigned long classidx = get_classification_index(lineAddr);

	unsigned long * localAlignedAddr = (unsigned long *)((char*)startAddr + lineAddr);
	unsigned long startIndex = getCacheIndex(lineAddr);
	unsigned long cacheIndex = getCacheIndex(alignedDistrAddr);
	cacheIndex = startIndex;
	alignedDistrAddr = lineAddr;

	unsigned long homenode = getHomenode(lineAddr);
	unsigned long offset = getOffset(lineAddr);
	unsigned long id = 1 << getID();
	unsigned long invid = ~id;

	pthread_mutex_lock(&cachemutex);

	/** @brief page is local */
	if(homenode == (getID())){
		int n;
		sem_wait(&ibsem);
		unsigned long sharers;
		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		unsigned long prevsharer = (globalSharers[classidx])&id;
		MPI_Win_unlock(workrank, sharerWindow);
		if(prevsharer != id){
			MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
			sharers = globalSharers[classidx];
			globalSharers[classidx] |= id;
			MPI_Win_unlock(workrank, sharerWindow);
			if(sharers != 0 && sharers != id && isPowerOf2(sharers)){
				unsigned long ownid = sharers&invid;
				unsigned long owner = workrank;
				for(n=0; n<numtasks; n++){
					if((unsigned long)(1<<n)==ownid){
						owner = n; //just get rank...
						break;
					}
				}
				if(owner==(unsigned long)workrank){
					throw "bad owner in local access";
				}
				else{
					/** @brief update remote private holder to shared */
					MPI_Win_lock(MPI_LOCK_SHARED, owner, 0, sharerWindow);
					MPI_Accumulate(&id, 1, MPI_LONG, owner, classidx,1,MPI_LONG,MPI_BOR,sharerWindow);
					MPI_Win_unlock(owner, sharerWindow);
				}
			}
			/**
			 *@brief set page to permit reads and map it to the page cache
			 *@todo Set cache offset to a variable instead of calculating it here
			 */
			localAlignedAddr = (unsigned long *)mmap(localAlignedAddr,pagesize*CACHELINE,PROT_READ,MAP_SHARED|MAP_FIXED,fd,cacheoffset+offset);
			if(localAlignedAddr== MAP_FAILED){
				printf("mmap failed in ArgoDSM address %p errno:%d - likely due to out of memory or too many memory mappings per process (check errno) \n",localAlignedAddr,errno);
				exit(EXIT_FAILURE);
			}

		}
		else{

			/** @brief get current sharers/writers and then add your own id */
			MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
			unsigned long sharers = globalSharers[classidx];
			unsigned long writers = globalSharers[classidx+1];
			globalSharers[classidx+1] |= id;
			MPI_Win_unlock(workrank, sharerWindow);

			/** @brief remote single writer */
			if(writers != id && writers != 0 && isPowerOf2(writers&invid)){
				int n;
				for(n=0; n<numtasks; n++){
					if(((unsigned long)(1<<n))==(writers&invid)){
						owner = n; //just get rank...
						break;
					}
				}
				MPI_Win_lock(MPI_LOCK_SHARED, owner, 0, sharerWindow);
				MPI_Accumulate(&id, 1, MPI_LONG, owner, classidx+1,1,MPI_LONG,MPI_BOR,sharerWindow);
				MPI_Win_unlock(owner, sharerWindow);
			}
			else if(writers == id || writers == 0){
				int n;
				for(n=0; n<numtasks; n++){
					if(n != workrank && ((1<<n)&sharers) != 0){
						MPI_Win_lock(MPI_LOCK_SHARED, n, 0, sharerWindow);
						MPI_Accumulate(&id, 1, MPI_LONG, n, classidx+1,1,MPI_LONG,MPI_BOR,sharerWindow);
						MPI_Win_unlock(n, sharerWindow);
					}
				}
			}
			/** @brief set page to permit read/write and map it to the page cache */
			localAlignedAddr =  (unsigned long *) mmap(localAlignedAddr,pagesize*CACHELINE,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,cacheoffset+offset);
			if(localAlignedAddr== MAP_FAILED){
				printf("mmap failed in ArgoDSM address %p errno:%d - likely due to out of memory or too many memory mappings per process (check errno) \n",localAlignedAddr,errno);
				exit(EXIT_FAILURE);
			}

		}
		sem_post(&ibsem);
		pthread_mutex_unlock(&cachemutex);
		return;
	}

	state  = cacheControl[startIndex].state;
	tag = cacheControl[startIndex].tag;

	if(state == INVALID || (tag != lineAddr && tag != GLOBAL_NULL)){
		if(loadline[0] == GLOBAL_NULL && prefetchline[0] != GLOBAL_NULL){
			if(prefetchline[0] <= startIndex && startIndex < prefetchline[0]+CACHELINE){
				loadline[0]= (startIndex+CACHELINE)%cachesize;
				loadtag[0]=lineAddr+pagesize*CACHELINE;
			}
			else{
				loadline[0]=cacheIndex%cachesize;
				loadtag[0]=alignedDistrAddr;
			}

			sem_post(&loadstartsem);
			sem_wait(&prefetchwaitsem);
			prefetchline[0] = GLOBAL_NULL;
			prefetchtag[0]= GLOBAL_NULL;
			pthread_mutex_unlock(&cachemutex);
			double t2 = MPI_Wtime();
			stats.loadtime+=t2-t1;
			return;
		}
		else if(prefetchline[0] == GLOBAL_NULL && loadline[0] != GLOBAL_NULL){
			if(loadline[0] <= startIndex && startIndex < loadline[0]+CACHELINE){
				prefetchline[0]=(startIndex+CACHELINE)%cachesize;
				prefetchtag[0]=lineAddr+pagesize*CACHELINE;
			}
			else{
				prefetchline[0]=cacheIndex%cachesize;
				prefetchtag[0]=alignedDistrAddr;
			}

			sem_post(&prefetchstartsem);
			sem_wait(&loadwaitsem);
			loadline[0] = GLOBAL_NULL;
			loadtag[0]=GLOBAL_NULL;
			pthread_mutex_unlock(&cachemutex);
			double t2 = MPI_Wtime();
			stats.loadtime+=t2-t1;
			return;
		}
		else{
			loadline[0]=startIndex%cachesize;
			loadtag[0]=alignedDistrAddr;
			sem_post(&loadstartsem);

#ifdef DUAL_LOAD
			prefetchline[0]=(startIndex+CACHELINE)%cachesize;
			prefetchtag[0]=alignedDistrAddr+CACHELINE*pagesize;
			sem_post(&prefetchstartsem);
			sem_wait(&loadwaitsem);
			loadline[0]=GLOBAL_NULL;
			loadtag[0]=GLOBAL_NULL;
#else
			sem_wait(&loadwaitsem);
			loadline[0]=GLOBAL_NULL;
			loadtag[0]=GLOBAL_NULL;
			prefetchline[0]=GLOBAL_NULL;
			prefetchtag[0]=GLOBAL_NULL;

#endif
			pthread_mutex_unlock(&cachemutex);
			double t2 = MPI_Wtime();
			stats.loadtime+=t2-t1;
			return;
		}
	}

	if(prefetchline[0] != GLOBAL_NULL){
		sem_wait(&prefetchwaitsem);
		prefetchline[0] = GLOBAL_NULL;
		prefetchtag[0]=GLOBAL_NULL;
		pthread_mutex_unlock(&cachemutex);
		double t2 = MPI_Wtime();
		stats.loadtime+=t2-t1;
		return;

	}
	else if(loadline[0] != GLOBAL_NULL){
		sem_wait(&loadwaitsem);
		loadline[0]=GLOBAL_NULL;
		loadtag[0] =GLOBAL_NULL;
		pthread_mutex_unlock(&cachemutex);
		double t2 = MPI_Wtime();
		stats.loadtime+=t2-t1;
		return;
	}

	unsigned long line = startIndex / CACHELINE;
	line *= CACHELINE;

	if(cacheControl[line].dirty == DIRTY){
		pthread_mutex_unlock(&cachemutex);
		return;
	}


	touchedcache[line] = 1;
	cacheControl[line].dirty = DIRTY;

	sem_wait(&ibsem);
	MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
	unsigned long writers = globalSharers[classidx+1];
	unsigned long sharers = globalSharers[classidx];
	MPI_Win_unlock(workrank, sharerWindow);
	/** @brief Either already registered write - or 1 or 0 other writers already cached */
	if(writers != id && isPowerOf2(writers)){
		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		globalSharers[classidx+1] |= id; //register locally
		MPI_Win_unlock(workrank, sharerWindow);

		/** @brief register and get latest sharers / writers */
		MPI_Win_lock(MPI_LOCK_SHARED, homenode, 0, sharerWindow);
		MPI_Get_accumulate(&id, 1,MPI_LONG,&writers,1,MPI_LONG,homenode,
											 classidx+1,1,MPI_LONG,MPI_BOR,sharerWindow);
		MPI_Get(&sharers,1, MPI_LONG, homenode, classidx, 1,MPI_LONG,sharerWindow);
		MPI_Win_unlock(homenode, sharerWindow);
		/* We get result of accumulation before operation so we need to account for that */
		writers |= id;
		/* Just add the (potentially) new sharers fetched to local copy */
		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		globalSharers[classidx] |= sharers;
		MPI_Win_unlock(workrank, sharerWindow);

		/** @brief check if we need to update */
		if(writers != id && writers != 0 && isPowerOf2(writers&invid)){
			int n;
			for(n=0; n<numtasks; n++){
				if(((unsigned long)(1<<n))==(writers&invid)){
					owner = n; //just get rank...
					break;
				}
			}
			MPI_Win_lock(MPI_LOCK_SHARED, owner, 0, sharerWindow);
			MPI_Accumulate(&id, 1, MPI_LONG, owner, classidx+1,1,MPI_LONG,MPI_BOR,sharerWindow);
			MPI_Win_unlock(owner, sharerWindow);
		}
		else if(writers==id || writers==0){
			int n;
			for(n=0; n<numtasks; n++){
				if(n != workrank && ((1<<n)&sharers) != 0){
					MPI_Win_lock(MPI_LOCK_SHARED, n, 0, sharerWindow);
					MPI_Accumulate(&id, 1, MPI_LONG, n, classidx+1,1,MPI_LONG,MPI_BOR,sharerWindow);
					MPI_Win_unlock(n, sharerWindow);
				}
			}
		}
	}
	sem_post(&ibsem);
	unsigned char * real = (unsigned char *)(localAlignedAddr);
	unsigned char * copy = (unsigned char *)(pagecopy + line*pagesize);
	memcpy(copy,real,CACHELINE*pagesize);
	addToWriteBuffer(startIndex);
	mprotect(localAlignedAddr, pagesize*CACHELINE,PROT_WRITE|PROT_READ);
	pthread_mutex_unlock(&cachemutex);
	double t2 = MPI_Wtime();
	stats.storetime += t2-t1;
	return;
}


unsigned long getHomenode(unsigned long addr){
	unsigned long homenode = addr/size_of_chunk;
	if(homenode >=(unsigned long)numtasks){
		exit(EXIT_FAILURE);
	}
	return homenode;
}

unsigned long getOffset(unsigned long addr){
	unsigned long offset = addr - (getHomenode(addr))*size_of_chunk;//offset in local memory on remote node (homenode
	if(offset >=size_of_chunk){
		exit(EXIT_FAILURE);
	}
	return offset;
}

void *writeloop(void * x){
	UNUSED_PARAM(x);
	unsigned long i;
	unsigned long oldstart;
	unsigned long idx,tag;

	while(1){

		sem_wait(&writerstartsem);
		sem_wait(&ibsem);

		oldstart = writebufferstart;
		i = oldstart;
		idx = writebuffer[i];
		tag = cacheControl[idx].tag;
		writebuffer[i] = GLOBAL_NULL;
		if(tag != GLOBAL_NULL && idx != GLOBAL_NULL && cacheControl[idx].dirty == DIRTY){
			mprotect((char*)startAddr+tag,CACHELINE*pagesize,PROT_READ);
			for(i = 0; i <CACHELINE; i++){
				storepageDIFF(idx+i,tag+pagesize*i);
				cacheControl[idx+i].dirty = CLEAN;
			}
		}
		for(i = 0; i < (unsigned long)numtasks; i++){
			if(barwindowsused[i] == 1){
				MPI_Win_unlock(i, globalDataWindow[i]);
				barwindowsused[i] = 0;
			}
		}
		writebufferstart = (writebufferstart+1)%writebuffersize;
		sem_post(&ibsem);
		sem_post(&writerwaitsem);
	}
}

void * loadcacheline(void * x){
	UNUSED_PARAM(x);
	int i;
	unsigned long homenode;
	unsigned long id = 1 << getID();
	unsigned long invid = ~id;
	while(1){
		sem_wait(&loadstartsem);
		if(loadtag[0]>=size_of_all){//Trying to access/prefetch out of memory
			sem_post(&loadwaitsem);
			continue;
		}
		homenode = getHomenode(loadtag[0]);
		unsigned long cacheIndex = loadline[0];
		if(cacheIndex >= cachesize){
			printf("idx > size   cacheIndex:%ld cachesize:%ld\n",cacheIndex,cachesize);
			sem_post(&loadwaitsem);
			continue;
		}
		sem_wait(&ibsem);


		unsigned long pageAddr = loadtag[0];
		unsigned long blocksize = pagesize*CACHELINE;
		unsigned long lineAddr = pageAddr/blocksize;
		lineAddr *= blocksize;

		unsigned long startidx = cacheIndex/CACHELINE;
		startidx*=CACHELINE;
		unsigned long end = startidx+CACHELINE;

		if(end>=cachesize){
			end = cachesize;
		}

		argo_byte tmpstate = cacheControl[startidx].state;
		unsigned long tmptag = cacheControl[startidx].tag;

		if(tmptag == lineAddr && tmpstate != INVALID){
			sem_post(&ibsem);
			sem_post(&loadwaitsem);
			continue;
		}


		void * lineptr = (char*)startAddr + lineAddr;

		if(cacheControl[startidx].tag  != lineAddr){
			if(cacheControl[startidx].tag  != lineAddr){
				if(pthread_mutex_trylock(&wbmutex) != 0){
					sem_post(&ibsem);
					pthread_mutex_lock(&wbmutex);
					sem_wait(&ibsem);
				}

				void * tmpptr2 = (char*)startAddr + cacheControl[startidx].tag;
				if(cacheControl[startidx].tag != GLOBAL_NULL && cacheControl[startidx].tag  != lineAddr){
					argo_byte dirty = cacheControl[startidx].dirty;
					if(dirty == DIRTY){
						mprotect(tmpptr2,blocksize,PROT_READ);
						int j;
						for(j=0; j < CACHELINE; j++){
							storepageDIFF(startidx+j,pagesize*j+(cacheControl[startidx].tag));
						}
					}

					for(i = 0; i < numtasks; i++){
						if(barwindowsused[i] == 1){
							MPI_Win_unlock(i, globalDataWindow[i]);
							barwindowsused[i] = 0;
						}
					}

					cacheControl[startidx].state = INVALID;
					cacheControl[startidx].tag = lineAddr;

					cacheControl[startidx].dirty=CLEAN;
					lineptr =  mmap(lineptr, blocksize, PROT_NONE,MAP_SHARED|MAP_FIXED,fd,pagesize*startidx);
					mprotect(tmpptr2,blocksize,PROT_NONE);
				}
				pthread_mutex_unlock(&wbmutex);
			}
		}



		stats.loads++;
		unsigned long classidx = get_classification_index(lineAddr);
		unsigned long tempsharer = 0;
		unsigned long tempwriter = 0;

		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		unsigned long prevsharer = (globalSharers[classidx])&id;
		MPI_Win_unlock(workrank, sharerWindow);
		int n;
		homenode = getHomenode(lineAddr);

		if(prevsharer==0 ){ //if there is strictly less than two 'stable' sharers
			MPI_Win_lock(MPI_LOCK_SHARED, homenode, 0, sharerWindow);
			MPI_Get_accumulate(&id, 1,MPI_LONG,&tempsharer,1,MPI_LONG,homenode,classidx,1,MPI_LONG,MPI_BOR,sharerWindow);
			MPI_Get(&tempwriter, 1,MPI_LONG,homenode,classidx+1,1,MPI_LONG,sharerWindow);
			MPI_Win_unlock(homenode, sharerWindow);
		}

		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		globalSharers[classidx] |= tempsharer;
		globalSharers[classidx+1] |= tempwriter;
		MPI_Win_unlock(workrank, sharerWindow);

		unsigned long offset = getOffset(lineAddr);
		if(isPowerOf2((tempsharer)&invid) && tempsharer != id && prevsharer == 0){ //Other private. but may not have loaded page yet.
			long ownid = (long)(tempsharer&invid);
			unsigned long owner;
			for(n=0; n<numtasks; n++){
				if((unsigned)(1<<n)==(unsigned)ownid){
					owner = n; //just get rank...
					break;
				}
			}
			MPI_Win_lock(MPI_LOCK_SHARED, owner, 0, sharerWindow);
			MPI_Accumulate(&id, 1, MPI_LONG, owner, classidx,1,MPI_LONG,MPI_BOR,sharerWindow);
			MPI_Win_unlock(owner, sharerWindow);

		}

		MPI_Win_lock(MPI_LOCK_SHARED, homenode , 0, globalDataWindow[homenode]);
		MPI_Get(&cacheData[startidx*pagesize],
						1,
						cacheblock,
						homenode,
						offset, 1,cacheblock,globalDataWindow[homenode]);
		MPI_Win_unlock(homenode, globalDataWindow[homenode]);

		if(cacheControl[startidx].tag == GLOBAL_NULL){
			lineptr =  mmap(lineptr, blocksize, PROT_READ,MAP_SHARED|MAP_FIXED,fd,pagesize*startidx);
			if(lineptr== MAP_FAILED){
				printf("MMAP failed %p errno:%d\n",lineptr,errno);
				exit(EXIT_FAILURE);
			}
			cacheControl[startidx].tag = lineAddr;
		}
		else{
			mprotect(lineptr,pagesize*CACHELINE,PROT_READ);
		}
		touchedcache[startidx] = 1;
		cacheControl[startidx].state = VALID;

		cacheControl[startidx].dirty=CLEAN;
		sem_post(&loadwaitsem);
		sem_post(&ibsem);

	}
}

void * prefetchcacheline(void * x){
	UNUSED_PARAM(x);
	int i;
	unsigned long homenode;
	unsigned long id = 1 << getID();
	unsigned long invid = ~id;
	while(1){
		sem_wait(&prefetchstartsem);
		if(prefetchtag[0]>=size_of_all){//Trying to access/prefetch out of memory
			sem_post(&prefetchwaitsem);
			continue;
		}


		homenode = getHomenode(prefetchtag[0]);
		unsigned long cacheIndex = prefetchline[0];
		if(cacheIndex >= cachesize){
			printf("idx > size   cacheIndex:%ld cachesize:%ld\n",cacheIndex,cachesize);
			sem_post(&prefetchwaitsem);
			continue;
		}


		sem_wait(&ibsem);
		unsigned long pageAddr = prefetchtag[0];
		unsigned long blocksize = pagesize*CACHELINE;
		unsigned long lineAddr = pageAddr/blocksize;
		lineAddr *= blocksize;
		unsigned long startidx = cacheIndex/CACHELINE;
		startidx*=CACHELINE;
		unsigned long end = startidx+CACHELINE;

		if(end>=cachesize){
			end = cachesize;
		}
		argo_byte tmpstate = cacheControl[startidx].state;
		unsigned long tmptag = cacheControl[startidx].tag;
		if(tmptag == lineAddr && tmpstate != INVALID){ //trying to load already valid ..
			sem_post(&ibsem);
			sem_post(&prefetchwaitsem);
			continue;
		}


		void * lineptr = (char*)startAddr + lineAddr;

		if(cacheControl[startidx].tag  != lineAddr){
			if(cacheControl[startidx].tag  != lineAddr){
				if(pthread_mutex_trylock(&wbmutex) != 0){
					sem_post(&ibsem);
					pthread_mutex_lock(&wbmutex);
					sem_wait(&ibsem);
				}

				void * tmpptr2 = (char*)startAddr + cacheControl[startidx].tag;
				if(cacheControl[startidx].tag != GLOBAL_NULL && cacheControl[startidx].tag  != lineAddr){
					argo_byte dirty = cacheControl[startidx].dirty;
					if(dirty == DIRTY){
						mprotect(tmpptr2,blocksize,PROT_READ);
						int j;
						for(j=0; j < CACHELINE; j++){
							storepageDIFF(startidx+j,pagesize*j+(cacheControl[startidx].tag));
						}
					}

					for(i = 0; i < numtasks; i++){
						if(barwindowsused[i] == 1){
							MPI_Win_unlock(i, globalDataWindow[i]);
							barwindowsused[i] = 0;
						}
					}


					cacheControl[startidx].state = INVALID;
					cacheControl[startidx].tag = lineAddr;
					cacheControl[startidx].dirty=CLEAN;

					lineptr =  mmap(lineptr, blocksize, PROT_NONE,MAP_SHARED|MAP_FIXED,fd,pagesize*startidx);
					mprotect(tmpptr2,blocksize,PROT_NONE);

				}
				pthread_mutex_unlock(&wbmutex);

			}
		}

		stats.loads++;
		unsigned long classidx = get_classification_index(lineAddr);
		unsigned long tempsharer = 0;
		unsigned long tempwriter = 0;
		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		unsigned long prevsharer = (globalSharers[classidx])&id;
		MPI_Win_unlock(workrank, sharerWindow);
		int n;
		homenode = getHomenode(lineAddr);

		if(prevsharer==0 ){ //if there is strictly less than two 'stable' sharers
			MPI_Win_lock(MPI_LOCK_SHARED, homenode, 0, sharerWindow);
			MPI_Get_accumulate(&id, 1,MPI_LONG,&tempsharer,1,MPI_LONG,homenode,classidx,1,MPI_LONG,MPI_BOR,sharerWindow);
			MPI_Get(&tempwriter, 1,MPI_LONG,homenode,classidx+1,1,MPI_LONG,sharerWindow);
			MPI_Win_unlock(homenode, sharerWindow);
		}

		MPI_Win_lock(MPI_LOCK_SHARED, workrank, 0, sharerWindow);
		globalSharers[classidx] |= tempsharer;
		globalSharers[classidx+1] |= tempwriter;
		MPI_Win_unlock(workrank, sharerWindow);

		unsigned long offset = getOffset(lineAddr);
		if(isPowerOf2((tempsharer)&invid) && prevsharer == 0){ //Other private. but may not have loaded page yet.
			unsigned long ownid = tempsharer&invid;
			unsigned long owner;
			for(n=0; n<numtasks; n++){
				if((unsigned)(1<<n)==(unsigned)ownid){
					owner = n; //just get rank...
					break;
				}
			}

			MPI_Win_lock(MPI_LOCK_SHARED, owner, 0, sharerWindow);
			MPI_Accumulate(&id, 1, MPI_LONG, owner, classidx,1,MPI_LONG,MPI_BOR,sharerWindow);
			MPI_Win_unlock(owner, sharerWindow);

		}

		MPI_Win_lock(MPI_LOCK_SHARED, homenode , 0, globalDataWindow[homenode]);
		MPI_Get(&cacheData[startidx*pagesize],1,cacheblock,homenode, offset, 1,cacheblock,globalDataWindow[homenode]);
		MPI_Win_unlock(homenode, globalDataWindow[homenode]);


		if(cacheControl[startidx].tag == GLOBAL_NULL){
			lineptr =  mmap(lineptr, blocksize, PROT_READ,MAP_SHARED|MAP_FIXED,fd,pagesize*startidx);
			if(lineptr== MAP_FAILED){
				printf("MMAP failed %p errno:%d\n",lineptr,errno);
				exit(EXIT_FAILURE);
			}
			cacheControl[startidx].tag = lineAddr;
		}
		else{
			mprotect(lineptr,pagesize*CACHELINE,PROT_READ);
		}

		touchedcache[startidx] = 1;
		cacheControl[startidx].state = VALID;
		cacheControl[startidx].dirty=CLEAN;
		sem_post(&prefetchwaitsem);
		sem_post(&ibsem);
	}
}


void initmpi(){
	int ret,initialized,thread_status;
	int thread_level = MPI_THREAD_SERIALIZED;
	MPI_Initialized(&initialized);
	if (!initialized){
		ret = MPI_Init_thread(NULL,NULL,thread_level,&thread_status);
	}
	else{
		printf("MPI was already initialized before starting ArgoDSM - shutting down\n");
		exit(EXIT_FAILURE);
	}

	if (ret != MPI_SUCCESS || thread_status != thread_level) {
		printf ("MPI not able to start properly\n");
		MPI_Abort(MPI_COMM_WORLD, ret);
		exit(EXIT_FAILURE);
	}

	MPI_Comm_size(MPI_COMM_WORLD,&numtasks);
	MPI_Comm_rank(MPI_COMM_WORLD,&rank);
	init_mpi_struct();
	init_mpi_cacheblock();
}

unsigned int getID(){
	return workrank;
}
unsigned int argo_get_nid(){
	return workrank;
}

unsigned int argo_get_nodes(){
	return numtasks;
}
unsigned int getThreadCount(){
	return NUM_THREADS;
}

//My sort of allocatefunction now since parmacs macros had this design
void * argo_gmalloc(unsigned long size){
	if(argo_get_nodes()==1){return malloc(size);}

	pthread_mutex_lock(&gmallocmutex);
	MPI_Barrier(workcomm);

	unsigned long roundedUp; //round up to number of pages to use.
	unsigned long currPage; //what pages has been allocated previously
	unsigned long alignment = pagesize*CACHELINE;

	roundedUp = size/(alignment);
	roundedUp = (alignment)*(roundedUp+1);

	currPage = (*allocationOffset)/(alignment);
	currPage = (alignment) *(currPage);

	if((*allocationOffset) +size > size_of_all){
		pthread_mutex_unlock(&gmallocmutex);
		return NULL;
	}

	void *ptrtmp = (char*)startAddr+*allocationOffset;
	*allocationOffset = (*allocationOffset) + roundedUp;

	if(ptrtmp == NULL){
		pthread_mutex_unlock(&gmallocmutex);
		exit(EXIT_FAILURE);
	}
	else{
		memset(ptrtmp,0,roundedUp);
	}
	swdsm_argo_barrier(1);
	pthread_mutex_unlock(&gmallocmutex);
	return ptrtmp;
}

void set_sighandler(){
	sigset_t sigs;
	struct sigaction sa;
	sigemptyset( &sigs );
	sigaddset( &sigs, SIGSEGV );

	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handler;
	sigaction(SIGSEGV, &sa, &old_sigaction);
}

void argo_initialize(unsigned long long size){
	int i;
	unsigned long j;
	initmpi();
	unsigned long alignment = pagesize*CACHELINE*numtasks;
	if((size%alignment)>0){
		size /= alignment;
		size++;
		size *= alignment;
	}

	startAddr = mmap((void *)(pagesize*2000000000L), size, PROT_NONE, MAP_ANON|MAP_SHARED, 0, 0);
	if(startAddr == (void *) -1){
		printf("startmap error  errno %d  tried to allocate :%llu bytes    startaddr:%lu\n",errno,size,pagesize*2000000000L);
		exit(EXIT_FAILURE);
	}

	threadbarrier = (pthread_barrier_t *) malloc(sizeof(pthread_barrier_t)*(NUM_THREADS+1));
	for(i = 1; i <= NUM_THREADS; i++){
		pthread_barrier_init(&threadbarrier[i],NULL,i);
	}

	cachesize = size+pagesize*CACHELINE;
	cachesize /= pagesize;
	cachesize /= CACHELINE;
	cachesize *= CACHELINE;

	classificationSize = 2*cachesize; // Could be smaller ?
	writebuffersize = WRITE_BUFFER_PAGES/CACHELINE;
	writebuffer = (unsigned long *) malloc(sizeof(unsigned long)*writebuffersize);
	for(i = 0; i < (int)writebuffersize; i++){
		writebuffer[i] = GLOBAL_NULL;
	}

	writebufferstart = 0;
	writebufferend = 0;

	barwindowsused = (char *)malloc(numtasks*sizeof(char));
	for(i = 0; i < numtasks; i++){
		barwindowsused[i] = 0;
	}

	prefetchline = (unsigned long *) malloc(sizeof(unsigned long)*numtasks);
	loadline = (unsigned long *) malloc(sizeof(unsigned long)*numtasks);
	prefetchtag = (unsigned long *) malloc(sizeof(unsigned long)*numtasks);
	loadtag = (unsigned long *) malloc(sizeof(unsigned long)*numtasks);

	int *workranks = (int *) malloc(sizeof(int)*numtasks);
	int *procranks = (int *) malloc(sizeof(int)*2);
	int workindex = 0;

	for(i = 0; i < numtasks; i++){
		prefetchline[i] = GLOBAL_NULL;
		loadline[i] = GLOBAL_NULL;
		workranks[workindex++] = i;
		procranks[0]=i;
		procranks[1]=i+1;
	}

	MPI_Comm_group(MPI_COMM_WORLD, &startgroup);
	MPI_Group_incl(startgroup,numtasks,workranks,&workgroup);
	MPI_Comm_create(MPI_COMM_WORLD,workgroup,&workcomm);
	MPI_Group_rank(workgroup,&workrank);

	if(size < pagesize*numtasks){
		size = pagesize*numtasks;
	}

	alignment = CACHELINE*pagesize;
	if(size % (alignment*numtasks) != 0){
		size = alignment*numtasks * (1+(size)/(alignment*numtasks));
	}

	//Allocate local memory for each node,
	size_of_all = size; //total distr. global memory
	GLOBAL_NULL=size_of_all+1;
	size_of_chunk = size/(numtasks); //part on each node
	set_sighandler();

	unsigned long cacheControlSize = sizeof(control_data)*cachesize;
	unsigned long gwritersize = classificationSize*sizeof(long);

	cacheControlSize /= pagesize;
	gwritersize /= pagesize;

	cacheControlSize +=1;
	gwritersize += 1;

	cacheControlSize *= pagesize;
	gwritersize *= pagesize;

	cacheoffset = pagesize*cachesize+cacheControlSize;
	globalData= (argo_byte *) memalign(pagesize,size_of_chunk);
	if(globalData == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	cacheData = (argo_byte*)memalign(pagesize, cachesize*pagesize);
	if(cacheData == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	cacheControl = (control_data *) memalign(pagesize,cacheControlSize);
	if(cacheControl == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	touchedcache = (argo_byte *)malloc(cachesize);
	if(touchedcache == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	lockbuffer = (unsigned long *) memalign(pagesize,pagesize);
	if(lockbuffer == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	pagecopy = (char*) memalign(pagesize,cachesize*pagesize);
	if(pagecopy == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	globalSharers = (unsigned long *)memalign(pagesize,gwritersize);
	if(globalSharers == NULL){
		printf("memalign error out of memory\n");
		exit(EXIT_FAILURE);
	}

	char processor_name[MPI_MAX_PROCESSOR_NAME];
	int name_len;
	MPI_Get_processor_name(processor_name, &name_len);

	sprintf(filename, "argocache%d", rank);
	fd = shm_open(filename,O_RDWR|O_CREAT,0664);
	MPI_Barrier(MPI_COMM_WORLD);
	unsigned long filesize = cachesize*pagesize*CACHELINE+cacheControlSize+size_of_chunk+pagesize+gwritersize+pagesize;
	if(ftruncate(fd,filesize)) {
		/// @todo this needs to be handled
		exit(EXIT_FAILURE);
	}

	void * tmpcache;
	tmpcache=cacheData;
	tmpcache =  mmap(tmpcache , pagesize*cachesize, PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
	if(tmpcache == (void *) -1){
		printf("1mmap error  errno%d\n",errno);
		exit(EXIT_FAILURE);
	}

	tmpcache=cacheControl;
	tmpcache =  mmap(tmpcache , cacheControlSize, PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,pagesize*cachesize );
	if(tmpcache == (void *) -1){
		printf("3mmap cachecontrol error  errno%d\n",errno);
		exit(EXIT_FAILURE);
	}

	tmpcache=globalData;
	tmpcache =  mmap(tmpcache , size_of_chunk, PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,pagesize*cachesize + cacheControlSize);
	if(tmpcache == (void *) -1){
		printf("4mmap error  errno%d  tmpacache:%p   size :%lu   offset:%lu\n",errno,tmpcache,size_of_chunk,pagesize*cachesize + cacheControlSize);
		exit(EXIT_FAILURE);
	}

	tmpcache=globalSharers;
	tmpcache =  mmap(tmpcache ,gwritersize , PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,pagesize*cachesize + cacheControlSize+size_of_chunk);
	if(tmpcache == (void *) -1){
		printf("global sharers mmap error  errno%d\n",errno);
		exit(EXIT_FAILURE);
	}

	tmpcache=lockbuffer;
	tmpcache =  mmap(tmpcache ,pagesize , PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,pagesize*cachesize + cacheControlSize+size_of_chunk+gwritersize);
	if(tmpcache == (void *) -1){
		printf("lockbuffer mmap error lockbuffer errno%d\n",errno);
		exit(EXIT_FAILURE);
	}

	sem_init(&loadwaitsem,0,0);
	sem_init(&loadstartsem,0,0);
	sem_init(&prefetchstartsem,0,0);
	sem_init(&prefetchwaitsem,0,0);

	sem_init(&writerstartsem,0,0);
	sem_init(&writerwaitsem,0,0);

	sem_init(&ibsem,0,1);
	sem_init(&globallocksem,0,1);

	allocationOffset = (unsigned long *)calloc(1,sizeof(unsigned long));
	globalDataWindow = (MPI_Win*)malloc(sizeof(MPI_Win)*numtasks);

	for(i = 0; i < numtasks; i++){
 		MPI_Win_create(globalData, size_of_chunk*sizeof(argo_byte), 1,
									 MPI_INFO_NULL, MPI_COMM_WORLD, &globalDataWindow[i]);
	}

	MPI_Win_create(globalSharers, gwritersize, sizeof(unsigned long),
								 MPI_INFO_NULL, MPI_COMM_WORLD, &sharerWindow);
	MPI_Win_create(lockbuffer, pagesize, 1, MPI_INFO_NULL, MPI_COMM_WORLD, &lockWindow);

	memset(pagecopy, 0, cachesize*pagesize);
	memset(touchedcache, 0, cachesize);
	memset(globalData, 0, size_of_chunk*sizeof(argo_byte));
	memset(cacheData, 0, cachesize*pagesize);
	memset(lockbuffer, 0, pagesize);
	memset(globalSharers, 0, gwritersize);
	memset(cacheControl, 0, cachesize*sizeof(control_data));

	for(j=0; j<cachesize; j++){
		cacheControl[j].tag = GLOBAL_NULL;
		cacheControl[j].state = INVALID;
		cacheControl[j].dirty = CLEAN;
	}

	pthread_create(&loadthread1,NULL,&loadcacheline,NULL);
	pthread_create(&loadthread2,NULL,&prefetchcacheline,(void*)NULL);
	pthread_create(&writethread,NULL,&writeloop,(void*)NULL);
	argo_reset_coherence(1);
}

void argo_finalize(){
	int i;
	swdsm_argo_barrier(1);
	if(getID() == 0){
		printf("ArgoDSM shutting down\n");
	}
	swdsm_argo_barrier(1);
	mprotect(startAddr,size_of_all,PROT_WRITE|PROT_READ);
	MPI_Barrier(MPI_COMM_WORLD);
	pthread_cancel(loadthread1);
	pthread_cancel(loadthread2);
	pthread_cancel(writethread);

	for(i=0; i <numtasks;i++){
		if(i==workrank){
			printStatistics();
		}
	}

	MPI_Barrier(MPI_COMM_WORLD);
	for(i=0; i<numtasks; i++){
		MPI_Win_free(&globalDataWindow[i]);
	}
	MPI_Win_free(&sharerWindow);
	MPI_Finalize();
	return;
}

void self_invalidation(){
	unsigned long i;
	double t1,t2;
	int flushed = 0;
	unsigned long id = 1 << getID();

	t1 = MPI_Wtime();
	for(i = 0; i < cachesize; i+=CACHELINE){
		if(touchedcache[i] != 0){
			unsigned long distrAddr = cacheControl[i].tag;
			unsigned long lineAddr = distrAddr/(CACHELINE*pagesize);
			lineAddr*=(pagesize*CACHELINE);
			unsigned long classidx = get_classification_index(lineAddr);
			argo_byte dirty = cacheControl[i].dirty;

			if(flushed == 0 && dirty == DIRTY){
				flushWriteBuffer();
				flushed = 1;
			}
			if(
				 // node is single writer
				 (globalSharers[classidx+1]==id)
				 ||
				 // No writer and assert that the node is a sharer
				 ((globalSharers[classidx+1]==0) && ((globalSharers[classidx]&id)==id))
				 ){
				touchedcache[i] =1;
				/*nothing - we keep the pages, SD is done in flushWB*/
			}
			else{ //multiple writer or SO
				cacheControl[i].dirty=CLEAN;
				cacheControl[i].state = INVALID;
				touchedcache[i] =0;
				mprotect((char*)startAddr + lineAddr, pagesize*CACHELINE, PROT_NONE);
			}
		}
	}
	t2 = MPI_Wtime();
	stats.selfinvtime += (t2-t1);
}

void swdsm_argo_barrier(int n){ //BARRIER
	double time1,time2;
	pthread_t barrierlockholder;
	time1 = MPI_Wtime();
	pthread_barrier_wait(&threadbarrier[n]);
	if(argo_get_nodes()==1){
		time2 = MPI_Wtime();
		stats.barriers++;
		stats.barriertime += (time2-time1);
		return;
	}

	if(pthread_mutex_trylock(&barriermutex) == 0){
		barrierlockholder = pthread_self();
		pthread_mutex_lock(&cachemutex);
		sem_wait(&ibsem);
		flushWriteBuffer();
		MPI_Barrier(workcomm);
		self_invalidation();
		sem_post(&ibsem);
		pthread_mutex_unlock(&cachemutex);
	}

	pthread_barrier_wait(&threadbarrier[n]);
	if(pthread_equal(barrierlockholder,pthread_self())){
		pthread_mutex_unlock(&barriermutex);
		time2 = MPI_Wtime();
		stats.barriers++;
		stats.barriertime += (time2-time1);
	}
}

void argo_reset_coherence(int n){
	int i;
	unsigned long j;
	stats.writebacks = 0;
	stats.stores = 0;
	memset(touchedcache, 0, cachesize);

	for(i=0;i<numtasks;i++){
		loadline[i] = GLOBAL_NULL;
		prefetchline[i] = GLOBAL_NULL;
	}
	sem_wait(&ibsem);
	MPI_Win_lock(MPI_LOCK_EXCLUSIVE, workrank, 0, sharerWindow);
	for(j = 0; j < classificationSize; j++){
		globalSharers[j] = 0;
	}
	MPI_Win_unlock(workrank, sharerWindow);
	sem_post(&ibsem);
	swdsm_argo_barrier(n);
	mprotect(startAddr,size_of_all,PROT_NONE);
	swdsm_argo_barrier(n);
	clearStatistics();
}

void argo_acquire(){
	int flag;
	pthread_mutex_lock(&cachemutex);
	sem_wait(&ibsem);
	self_invalidation();
	MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,workcomm,&flag,MPI_STATUS_IGNORE);
	sem_post(&ibsem);
	pthread_mutex_unlock(&cachemutex);
}


void argo_release(){
	pthread_mutex_lock(&cachemutex);
	sem_wait(&ibsem);
	flushWriteBuffer();
	sem_post(&ibsem);
	pthread_mutex_unlock(&cachemutex);
}

void argo_acq_rel(){
	argo_acquire();
	argo_release();
}

double argo_wtime(){
	return MPI_Wtime();
}

void clearStatistics(){
	stats.selfinvtime = 0;
	stats.loadtime = 0;
	stats.storetime = 0;
	stats.flushtime = 0;
	stats.writebacktime = 0;
	stats.locktime=0;
	stats.barriertime = 0;
	stats.stores = 0;
	stats.writebacks = 0;
	stats.loads = 0;
	stats.barriers = 0;
	stats.locks = 0;
}

void storepageDIFF(unsigned long index, unsigned long addr){
	unsigned int i,j;
	int cnt = 0;
	unsigned long homenode = getHomenode(addr);
	unsigned long offset = getOffset(addr);

	char * copy = (char *)(pagecopy + index*pagesize);
	char * real = (char *)startAddr+addr;
	size_t drf_unit = sizeof(char);

	if(barwindowsused[homenode] == 0){
		MPI_Win_lock(MPI_LOCK_EXCLUSIVE, homenode, 0, globalDataWindow[homenode]);
		barwindowsused[homenode] = 1;
	}

	for(i = 0; i < pagesize; i+=drf_unit){
		int branchval;
		for(j=i; j < i+drf_unit; j++){
			branchval = real[j] != copy[j];
			if(branchval != 0){
				break;
			}
		}
		if(branchval != 0){
			cnt+=drf_unit;
		}
		else{
			if(cnt > 0){
				MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, offset+((i)-cnt), cnt, MPI_BYTE, globalDataWindow[homenode]);
				cnt = 0;
			}
		}
	}
	if(cnt > 0){
		MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, offset+((i)-cnt), cnt, MPI_BYTE, globalDataWindow[homenode]);
	}
	stats.stores++;
}

void printStatistics(){
	printf("#####################STATISTICS#########################\n");
	printf("# PROCESS ID %d \n",workrank);
	printf("cachesize:%ld,CACHELINE:%ld wbsize:%ld\n",cachesize,CACHELINE,writebuffersize);
	printf("     writebacktime+=(t2-t1): %lf\n",stats.writebacktime);
	printf("# Storetime : %lf , loadtime :%lf flushtime:%lf, writebacktime: %lf\n",stats.storetime,stats.loadtime,stats.flushtime, stats.writebacktime);
	printf("# Barriertime : %lf, selfinvtime %lf\n",stats.barriertime, stats.selfinvtime);
	printf("stores:%lu, loads:%lu, barriers:%lu\n",stats.stores,stats.loads,stats.barriers);
	printf("Locks:%d\n",stats.locks);
	printf("########################################################\n");
	printf("\n\n");
}

void *argo_get_global_base(){return startAddr;}
size_t argo_get_global_size(){return size_of_all;}

inline unsigned long get_classification_index(uint64_t addr){
	return (2*(addr/(pagesize*CACHELINE))) % classificationSize;
}
