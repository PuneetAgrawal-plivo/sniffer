#ifndef PCAP_QUEUE_H
#define PCAP_QUEUE_H


#include <memory.h>
#include <netdb.h>
#include <pthread.h>
#include <pcap.h>
#include <deque>
#include <queue>
#include <string>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/syscall.h>

#include "pcap_queue_block.h"
#include "md5.h"
#include "sniff.h"
#include "pstat.h"
#include "ip_frag.h"

#define READ_THREADS_MAX 20
#define DLT_TYPES_MAX 10
#define PCAP_QUEUE_NEXT_THREADS_MAX 3

class pcap_block_store_queue {
public:
	pcap_block_store_queue();
	~pcap_block_store_queue();
	void push(pcap_block_store* blockStore) {
		if(this->queueBlock->push(&blockStore, true)) {
			this->add_sizeOfBlocks(blockStore->getUseSize());
		}
	}
	pcap_block_store* pop(bool removeFromFront = true, size_t blockSize = 0) {
		pcap_block_store* blockStore = NULL;
		if(this->queueBlock->get(&blockStore)) {
			if(removeFromFront) {
				this->queueBlock->moveReadit();
			}
		}
		if(blockStore && removeFromFront) {
			this->sub_sizeOfBlocks(blockSize ? blockSize : blockStore->getUseSize());
		}
		return(blockStore);
	}
	size_t getUseItems() {
		return(this->countOfBlocks);
	}	
	uint64_t getUseSize() {
		return(this->sizeOfBlocks);
	}
private:
	void add_sizeOfBlocks(size_t size) {
		__sync_fetch_and_add(&this->sizeOfBlocks, size);
		__sync_fetch_and_add(&this->countOfBlocks, 1);
	}
	void sub_sizeOfBlocks(size_t size) {
		__sync_fetch_and_sub(&this->sizeOfBlocks, size);
		__sync_fetch_and_sub(&this->countOfBlocks, 1);
	}
private:
	rqueue_quick<pcap_block_store*> *queueBlock;
	volatile size_t countOfBlocks;
	volatile size_t sizeOfBlocks;
};

class pcap_file_store {
public:
	enum eTypeHandle {
		typeHandlePush 	= 1,
		typeHandlePop 	= 2,
		typeHandleAll 	= 4
	};
public:
	pcap_file_store(u_int id = 0, const char *folder = NULL);
	~pcap_file_store();
	bool push(pcap_block_store *blockStore);
	bool pop(pcap_block_store *blockStore);
	bool isFull(bool forceSetFull = false) {
		if(this->full) {
			return(true);
		}
		extern size_t opt_pcap_queue_file_store_max_size;
		extern u_int opt_pcap_queue_file_store_max_time_ms;
		if(this->fileSize >= opt_pcap_queue_file_store_max_size ||
		   (this->fileSize && getTimeMS_rdtsc() > (this->timestampMS + opt_pcap_queue_file_store_max_time_ms)) ||
		   (this->fileSize && forceSetFull)) {
			this->close(typeHandlePush);
			this->full = true;
			return(true);
		}
		return(false);
	}
	bool isForDestroy() {
		return(this->full &&
		       this->countPush == this->countPop);
	}
	std::string getFilePathName();
private:
	bool open(eTypeHandle typeHandle);
	bool close(eTypeHandle typeHandle);
	bool destroy();
	void lock_sync_flush_file() {
		while(__sync_lock_test_and_set(&this->_sync_flush_file, 1));
	}
	void unlock_sync_flush_file() {
		__sync_lock_release(&this->_sync_flush_file);
	}
private:
	u_int id;
	std::string folder;
	FILE *fileHandlePush;
	FILE *fileHandlePop;
	u_char *fileBufferPush;
	u_char *fileBufferPop;
	size_t fileSize;
	size_t fileSizeFlushed;
	size_t countPush;
	size_t countPop;
	bool full;
	u_long timestampMS;
	volatile int _sync_flush_file;
friend class pcap_store_queue;
};

class pcap_store_queue {
public:
	pcap_store_queue(const char *fileStoreFolder);
	~pcap_store_queue();
	bool push(pcap_block_store *blockStore, bool deleteBlockStoreIfFail = true);
	bool pop(pcap_block_store **blockStore);
	size_t getQueueSize() {
		return(this->queueStore.size());
	}
private:
	pcap_file_store *findFileStoreById(u_int id);
	void cleanupFileStore();
	uint64_t getFileStoreUseSize(bool lock = true);
	void lock_queue() {
		while(__sync_lock_test_and_set(&this->_sync_queue, 1));
	}
	void unlock_queue() {
		__sync_lock_release(&this->_sync_queue);
	}
	void lock_fileStore() {
		while(__sync_lock_test_and_set(&this->_sync_fileStore, 1));
	}
	void unlock_fileStore() {
		__sync_lock_release(&this->_sync_fileStore);
	}
	void add_sizeOfBlocksInMemory(size_t size) {
		extern cBuffersControl buffersControl;
		buffersControl.add__pcap_store_queue__sizeOfBlocksInMemory(size);
	}
	void sub_sizeOfBlocksInMemory(size_t size) {
		extern cBuffersControl buffersControl;
		buffersControl.sub__pcap_store_queue__sizeOfBlocksInMemory(size);
	}
private:
	std::string fileStoreFolder;
	std::deque<pcap_block_store*> queueStore;
	std::deque<pcap_file_store*> fileStore;
	u_int lastFileStoreId;
	volatile int _sync_queue;
	volatile int _sync_fileStore;
	int cleanupFileStoreCounter;
	u_long lastTimeLogErrDiskIsFull;
	u_long lastTimeLogErrMemoryIsFull;
friend class PcapQueue_readFromFifo;
};

class PcapQueue {
public:
	enum eTypeQueue {
		readFromInterface,
		readFromFifo
	};
	enum eTypeThread {
		mainThread,
		writeThread,
		nextThread1,
		nextThread2,
		nextThread3
	};
	PcapQueue(eTypeQueue typeQueue, const char *nameQueue);
	virtual ~PcapQueue();
	void setFifoFileForRead(const char *fifoFileForRead);
	void setFifoFileForWrite(const char *fifoFileForWrite);
	void setFifoReadHandle(int fifoReadHandle);
	void setFifoWriteHandle(int fifoWriteHandle);
	void setEnableMainThread(bool enable = true);
	void setEnableWriteThread(bool enable = true);
	void setEnableAutoTerminate(bool enableAutoTerminate);
	bool start();
	virtual void terminate();
	bool isInitOk();
	bool isTerminated();
	void setInstancePcapHandle(PcapQueue *pcapQueue);
	void setInstancePcapFifo(class PcapQueue_readFromFifo *pcapQueue);
	inline pcap_t* getPcapHandle(int dlt);
	void pcapStat(int statPeriod = 1, bool statCalls = true);
	string pcapDropCountStat();
	void initStat();
	void getThreadCpuUsage(bool writeThread = false);
protected:
	virtual bool createThread();
	virtual bool createMainThread();
	virtual bool createWriteThread();
	inline int pcap_next_ex_queue(pcap_t* pcapHandle, pcap_pkthdr** header, u_char** packet);
	inline int readPcapFromFifo(pcap_pkthdr_plus *header, u_char **packet, bool usePacketBuffer = false);
	bool writePcapToFifo(pcap_pkthdr_plus *header, u_char *packet);
	virtual bool init() { return(true); };
	virtual bool initThread(void *arg, unsigned int arg2, string *error);
	virtual bool initWriteThread(void *arg, unsigned int arg2);
	virtual void *threadFunction(void *arg, unsigned int arg2) = 0;
	virtual void *writeThreadFunction(void *arg, unsigned int arg2) { return(NULL); }
	virtual bool openFifoForRead(void *arg, unsigned int arg2);
	virtual bool openFifoForWrite(void *arg, unsigned int arg2);
	virtual pcap_t* _getPcapHandle(int dlt) { 
		extern pcap_t *global_pcap_handle;
		return(global_pcap_handle); 
	}
	virtual string pcapStatString_packets(int statPeriod);
	virtual double pcapStat_get_compress();
	virtual double pcapStat_get_speed_mb_s(int statPeriod);
	virtual string pcapStatString_bypass_buffer(int statPeriod) { return(""); }
	virtual unsigned long pcapStat_get_bypass_buffer_size_exeeded() { return(0); }
	virtual string pcapStatString_memory_buffer(int statPeriod) { return(""); }
	virtual string pcapStatString_disk_buffer(int statPeriod) { return(""); }
	virtual double pcapStat_get_disk_buffer_perc() { return(-1); }
	virtual double pcapStat_get_disk_buffer_mb() { return(-1); }
	virtual string pcapStatString_interface(int statPeriod) { return(""); }
	virtual string pcapDropCountStat_interface() { return(""); }
	virtual ulong getCountPacketDrop() { return(0); }
	virtual string getStatPacketDrop() { return(""); }
	virtual string pcapStatString_cpuUsageReadThreads(double *sumMax = NULL) { if(sumMax) *sumMax = 0; return(""); };
	virtual void initStat_interface() {};
	int getThreadPid(eTypeThread typeThread);
	pstat_data *getThreadPstatData(eTypeThread typeThread);
	void preparePstatData(eTypeThread typeThread = mainThread);
	void prepareProcPstatData();
	double getCpuUsagePerc(eTypeThread typeThread = mainThread, bool preparePstatData = false);
	virtual string getCpuUsage(bool writeThread = false, bool preparePstatData = false) { return(""); }
	long unsigned int getVsizeUsage(bool preparePstatData = false);
	long unsigned int getRssUsage(bool preparePstatData = false);
	virtual bool isMirrorSender() {
		return(false);
	}
	virtual bool isMirrorReceiver() {
		return(false);
	}
	inline void processBeforeAddToPacketBuffer(pcap_pkthdr* header,u_char* packet, u_int offset);
protected:
	eTypeQueue typeQueue;
	std::string nameQueue;
	pthread_t threadHandle;
	pthread_t writeThreadHandle;
	std::string fifoFileForRead;
	std::string fifoFileForWrite;
	bool enableMainThread;
	bool enableWriteThread;
	bool enableAutoTerminate;
	int fifoReadHandle;
	int fifoWriteHandle;
	bool threadInitOk;
	bool threadInitFailed;
	bool writeThreadInitOk;
	bool threadTerminated;
	bool writeThreadTerminated;
	bool threadDoTerminate;
	int mainThreadId;
	int writeThreadId;
	int nextThreadsId[PCAP_QUEUE_NEXT_THREADS_MAX];
	pstat_data mainThreadPstatData[2];
	pstat_data writeThreadPstatData[2];
	pstat_data nextThreadsPstatData[PCAP_QUEUE_NEXT_THREADS_MAX][2];
	pstat_data procPstatData[2];
	bool initAllReadThreadsFinished;
protected:
	class PcapQueue_readFromFifo *instancePcapFifo;
private:
	u_char* packetBuffer;
	PcapQueue *instancePcapHandle;
	u_int64_t counter_calls_old;
	u_int64_t counter_calls_clean_old;
	u_int64_t counter_sip_packets_old[2];
	u_int64_t counter_sip_register_packets_old;
	u_int64_t counter_sip_message_packets_old;
	u_int64_t counter_rtp_packets_old;
	u_int64_t counter_all_packets_old;
	u_long lastTimeLogErrPcapNextExNullPacket;
	u_long lastTimeLogErrPcapNextExErrorReading;
friend void *_PcapQueue_threadFunction(void *arg);
friend void *_PcapQueue_writeThreadFunction(void *arg);
};

struct pcapProcessData {
	pcapProcessData() {
		memset(this, 0, sizeof(pcapProcessData) - sizeof(ipfrag_data_s));
		extern int opt_dup_check;
		if(opt_dup_check) {
			this->prevmd5s = new FILE_LINE unsigned char[65536 * MD5_DIGEST_LENGTH]; // 1M
			memset(this->prevmd5s, 0, 65536 * MD5_DIGEST_LENGTH * sizeof(unsigned char));
		}
	}
	~pcapProcessData() {
		if(this->prevmd5s) {
			delete [] this->prevmd5s;
		}
		ipfrag_prune(0, 1, &ipfrag_data);
	}
	sll_header *header_sll;
	ether_header *header_eth;
	iphdr2 *header_ip;
	tcphdr2 *header_tcp;
	udphdr2 *header_udp;
	udphdr2 header_udp_tmp;
	int protocol;
	u_int header_ip_offset;
	char *data;
	int datalen;
	int traillen;
	int istcp;
	uint16_t md5[MD5_DIGEST_LENGTH / (sizeof(uint16_t) / sizeof(unsigned char))];
	unsigned char *prevmd5s;
	MD5_CTX ctx;
	u_int ipfrag_lastprune;
	ipfrag_data_s ipfrag_data;
};


class PcapQueue_readFromInterface_base {
public:
	PcapQueue_readFromInterface_base(const char *interfaceName = NULL);
	virtual ~PcapQueue_readFromInterface_base();
	void setInterfaceName(const char *interfaceName);
protected:
	virtual bool startCapture(string *error);
	inline int pcap_next_ex_iface(pcap_t *pcapHandle, pcap_pkthdr** header, u_char** packet);
	inline int pcap_dispatch(pcap_t *pcapHandle);
	inline int pcapProcess(pcap_pkthdr** header, u_char** packet, bool *destroy, 
			       bool enableDefrag = true, bool enableCalcMD5 = true, bool enableDedup = true, bool enableDump = true);
	virtual string pcapStatString_interface(int statPeriod);
	virtual string pcapDropCountStat_interface();
	virtual ulong getCountPacketDrop();
	virtual string getStatPacketDrop();
	virtual void initStat_interface();
	virtual string getInterfaceName(bool simple = false);
protected:
	string interfaceName;
	bpf_u_int32 interfaceNet;
	bpf_u_int32 interfaceMask;
	pcap_t *pcapHandle;
	queue<pcap_t*> pcapHandlesLapsed;
	bool pcapEnd;
	bpf_program filterData;
	bool filterDataUse;
	pcap_dumper_t *pcapDumpHandle;
	u_int64_t pcapDumpLength;
	int pcapLinklayerHeaderType;
	size_t pcap_snaplen;
	pcapProcessData ppd;
private:
	int pcap_promisc;
	int pcap_timeout;
	int pcap_buffer_size;
	u_int _last_ps_drop;
	u_int _last_ps_ifdrop;
	u_long countPacketDrop;
	u_int64_t lastPacketTimeUS;
	u_long lastTimeLogErrPcapNextExNullPacket;
	u_long lastTimeLogErrPcapNextExErrorReading;
};

struct sHeaderPacket {
	sHeaderPacket(pcap_pkthdr *header = NULL, u_char *packet = NULL) {
		this->header = header;
		this->packet = packet;
	}
	inline void alloc(size_t snaplen) {
		header = new FILE_LINE pcap_pkthdr;
		packet = new FILE_LINE u_char[snaplen];
	}
	inline void free() {
		if(header) {
			delete header;
			header = NULL;
		}
		if(packet) {
			delete [] packet;
			packet = NULL;
		}
	}
	pcap_pkthdr *header;
	u_char *packet;
};

#define PcapQueue_HeaderPacketStack_add_max 5
#define PcapQueue_HeaderPacketStack_hp_max 100
class PcapQueue_HeaderPacketStack {
private:
	struct sHeaderPacketPool {
		void free_all() {
			for(u_int i = 0; i < PcapQueue_HeaderPacketStack_hp_max; i++) {
				hp[i].free();
			}
		}
		sHeaderPacket hp[PcapQueue_HeaderPacketStack_hp_max];
	};
public:
	PcapQueue_HeaderPacketStack(unsigned int size) {
		for(int ia = 0; ia < PcapQueue_HeaderPacketStack_add_max; ia++) {
			hpp_add_size[ia] = 0;
		}
		hpp_get_size = 0;
		stack = new rqueue_quick<sHeaderPacketPool>(size, 0, 0, NULL, false, __FILE__, __LINE__);
	}
	~PcapQueue_HeaderPacketStack() {
		for(int ia = 0; ia < PcapQueue_HeaderPacketStack_add_max; ia++) {
			for(u_int i = 0; i < hpp_add_size[ia]; i++) {
				hpp_add[ia].hp[i].free();
			}
		}
		for(u_int i = 0; i < hpp_get_size; i++) {
			hpp_get.hp[PcapQueue_HeaderPacketStack_hp_max - i - 1].free();
		}
		sHeaderPacket headerPacket;
		while(get_hp(&headerPacket)) {
			headerPacket.free();
		}
		delete stack;
	}
	bool add_hp(sHeaderPacket *headerPacket, int ia) {
		if(hpp_add_size[ia] == PcapQueue_HeaderPacketStack_hp_max) {
			if(stack->push(&hpp_add[ia], false, true)) {
				hpp_add[ia].hp[0] = *headerPacket;
				hpp_add_size[ia] = 1;
				return(true);
			}
		} else {
			hpp_add[ia].hp[hpp_add_size[ia]] = *headerPacket;
			++hpp_add_size[ia];
			return(true);
		}
		return(false);
	}
	bool get_hp(sHeaderPacket *headerPacket) {
		if(hpp_get_size) {
			*headerPacket = hpp_get.hp[PcapQueue_HeaderPacketStack_hp_max - hpp_get_size];
			--hpp_get_size;
			return(true);
		} else {
			if(stack->pop(&hpp_get, false)) {
				*headerPacket = hpp_get.hp[0];
				hpp_get_size = PcapQueue_HeaderPacketStack_hp_max - 1;
				return(true);
			}
		}
		return(false);
	}
private:
	sHeaderPacketPool hpp_add[PcapQueue_HeaderPacketStack_add_max];
	u_int hpp_add_size[PcapQueue_HeaderPacketStack_add_max];
	sHeaderPacketPool hpp_get;
	u_int hpp_get_size;
	rqueue_quick<sHeaderPacketPool> *stack;
};

class PcapQueue_readFromInterfaceThread : protected PcapQueue_readFromInterface_base {
public:
	enum eTypeInterfaceThread {
		read,
		defrag,
		md1,
		md2,
		dedup
	};
	struct hpi {
		pcap_pkthdr* header;
		u_char* packet;
		bool ok_for_header_packet_stack;
		u_int offset;
		uint16_t md5[MD5_DIGEST_LENGTH / (sizeof(uint16_t) / sizeof(unsigned char))];
	};
	struct hpi_batch {
		hpi_batch(uint32_t max_count) {
			this->max_count = max_count;
			this->hpis = new FILE_LINE hpi[max_count];
			count = 0;
			used = 0;
		}
		~hpi_batch() {
			delete [] hpis;
		}
		uint32_t max_count;
		hpi *hpis;
		volatile uint32_t count;
		volatile unsigned char used;
	};
	PcapQueue_readFromInterfaceThread(const char *interfaceName, eTypeInterfaceThread typeThread = read,
					  PcapQueue_readFromInterfaceThread *readThread = NULL,
					  PcapQueue_readFromInterfaceThread *prevThread = NULL);
	~PcapQueue_readFromInterfaceThread();
protected:
	inline void push(pcap_pkthdr* header,u_char* packet, bool ok_for_header_packet_stack,
			 u_int offset, uint16_t *md5);
	inline hpi pop();
	inline hpi POP();
	u_int64_t getTime_usec() {
		if(!readIndex) {
			unsigned int _readIndex = readit % qringmax;
			if(qring[_readIndex]->used) {
				readIndex = _readIndex + 1;
				readIndexPos = 0;
				readIndexCount = qring[_readIndex]->count;
			}
		}
		if(readIndex && readIndexCount && readIndexPos < readIndexCount) {
			return(this->qring[readIndex - 1]->hpis[readIndexPos].header->ts.tv_sec * 1000000ull + 
			       this->qring[readIndex - 1]->hpis[readIndexPos].header->ts.tv_usec);
		}
		return(0);
	}
	u_int64_t getTIME_usec() {
		return(this->dedupThread ? this->dedupThread->getTime_usec() : this->getTime_usec());
	}
	bool isTerminated() {
		return(this->threadTerminated);
	}
private:
	void *threadFunction(void *arg, unsigned int arg2);
	void preparePstatData();
	double getCpuUsagePerc(bool preparePstatData = false);
	double getQringFillingPerc() {
		unsigned int _readit = readit;
		unsigned int _writeit = writeit;
		return(_writeit >= _readit ?
			(double)(_writeit - _readit) / qringmax * 100 :
			(double)(qringmax - _readit + _writeit) / qringmax * 100);
	}
	string getQringFillingPercStr();
	void terminate();
private:
	pthread_t threadHandle;
	int threadId;
	int threadInitOk;
	bool threadInitFailed;
	hpi_batch **qring;
	unsigned int qringmax;
	volatile unsigned int readit;
	volatile unsigned int writeit;
	unsigned int readIndex;
	unsigned int readIndexPos;
	unsigned int readIndexCount;
	unsigned int writeIndex;
	unsigned int writeIndexCount;
	unsigned int counter;
	bool threadTerminated;
	pstat_data threadPstatData[2];
	volatile int _sync_qring;
	eTypeInterfaceThread typeThread;
	PcapQueue_readFromInterfaceThread *readThread;
	PcapQueue_readFromInterfaceThread *defragThread;
	PcapQueue_readFromInterfaceThread *md1Thread;
	PcapQueue_readFromInterfaceThread *md2Thread;
	PcapQueue_readFromInterfaceThread *dedupThread;
	PcapQueue_readFromInterfaceThread *prevThread;
	bool threadDoTerminate;
	PcapQueue_HeaderPacketStack *headerPacketStack;
friend void *_PcapQueue_readFromInterfaceThread_threadFunction(void *arg);
friend class PcapQueue_readFromInterface;
};

class PcapQueue_readFromInterface : public PcapQueue, protected PcapQueue_readFromInterface_base {
private:
	struct delete_packet_info {
		pcap_pkthdr *header;
		u_char *packet;
		bool ok_for_header_packet_stack;
		int read_thread_index;
	};
public:
	PcapQueue_readFromInterface(const char *nameQueue);
	virtual ~PcapQueue_readFromInterface();
	void setInterfaceName(const char *interfaceName);
	void terminate();
	bool openPcap(const char *filename);
	bool isPcapEnd() {
		return(this->pcapEnd);
	}
protected:
	bool init();
	bool initThread(void *arg, unsigned int arg2, string *error);
	void *threadFunction(void *arg, unsigned int arg2);
	void *writeThreadFunction(void *arg, unsigned int arg2);
	bool openFifoForWrite(void *arg, unsigned int arg2);
	bool startCapture(string *error);
	pcap_t* _getPcapHandle(int dlt) { 
		return(this->pcapHandle);
	}
	string pcapStatString_bypass_buffer(int statPeriod);
	unsigned long pcapStat_get_bypass_buffer_size_exeeded();
	string pcapStatString_interface(int statPeriod);
	string pcapDropCountStat_interface();
	virtual ulong getCountPacketDrop();
	virtual string getStatPacketDrop();
	void initStat_interface();
	string pcapStatString_cpuUsageReadThreads(double *sumMax = NULL);
	string getInterfaceName(bool simple = false);
private:
	inline void check_bypass_buffer();
	inline void delete_header_packet(pcap_pkthdr *header, u_char *packet, int read_thread_index, int packetStackIndex);
protected:
	pcap_dumper_t *fifoWritePcapDumper;
	PcapQueue_readFromInterfaceThread *readThreads[READ_THREADS_MAX];
	int readThreadsCount;
	u_long lastTimeLogErrThread0BufferIsFull;
private:
	rqueue_quick<pcap_block_store*> *block_qring;
};

class PcapQueue_readFromFifo : public PcapQueue {
public:
	enum ePacketServerDirection {
		directionNA,
		directionRead,
		directionWrite
	};
	struct sPacketServerConnection {
		sPacketServerConnection(int socketClient, sockaddr_in &socketClientInfo, PcapQueue_readFromFifo *parent, unsigned int id) {
			this->socketClient = socketClient;
			this->socketClientInfo = socketClientInfo;
			this->parent = parent;
			this->id = id;
			this->active = false;
			this->threadHandle = 0;
			this->threadId = 0;
			memset(this->threadPstatData, 0, sizeof(this->threadPstatData));
		}
		~sPacketServerConnection() {
			if(this->socketClient) {
				close(this->socketClient);
			}
		}
		int socketClient;
		sockaddr_in socketClientInfo;
		string socketClientIP;
		PcapQueue_readFromFifo *parent;
		unsigned int id;
		bool active;
		pthread_t threadHandle;
		int threadId;
		pstat_data threadPstatData[2];
	};
	struct sPacketTimeInfo {
		pcap_block_store *blockStore;
		size_t blockStoreIndex;
		pcap_pkthdr_plus *header;
		u_char *packet;
		u_int64_t utime;
		u_int64_t at;
	};
	struct sBlockInfo {
		pcap_block_store *blockStore;
		size_t count_processed;
		u_int64_t utime_first;
		u_int64_t utime_last;
		u_int64_t at;
	};
public:
	PcapQueue_readFromFifo(const char *nameQueue, const char *fileStoreFolder);
	virtual ~PcapQueue_readFromFifo();
	void setPacketServer(ip_port ipPort, ePacketServerDirection direction);
	size_t getQueueSize() {
		return(this->pcapStoreQueue.getQueueSize());
	}
	inline void addBlockStoreToPcapStoreQueue(pcap_block_store *blockStore);
protected:
	bool createThread();
	bool createSocketServerThread();
	bool initThread(void *arg, unsigned int arg2, string *error);
	void *threadFunction(void *arg, unsigned int arg2);
	void *writeThreadFunction(void *arg, unsigned int arg2);
	bool openFifoForRead(void *arg, unsigned int arg2);
	bool openFifoForWrite(void *arg, unsigned int arg2);
	bool openPcapDeadHandle(int dlt);
	pcap_t* _getPcapHandle(int dlt) {
		extern pcap_t *global_pcap_handle;
		if(this->pcapDeadHandles_count) {
			if(!dlt) {
				return(this->pcapDeadHandles[0]);
			}
			for(int i = 0; i < this->pcapDeadHandles_count; i++) {
				if(this->pcapDeadHandles_dlt[i] == dlt) {
					return(this->pcapDeadHandles[i]);
				}
			}
			if(openPcapDeadHandle(dlt)) {
				return(this->pcapDeadHandles[this->pcapDeadHandles_count - 1]);
			} else {
				return(NULL);
			}
		}
		return(this->fifoReadPcapHandle ? this->fifoReadPcapHandle : global_pcap_handle);
	}
	string pcapStatString_memory_buffer(int statPeriod);
	double pcapStat_get_memory_buffer_perc();
	double pcapStat_get_memory_buffer_perc_trash();
	string pcapStatString_disk_buffer(int statPeriod);
	double pcapStat_get_disk_buffer_perc();
	double pcapStat_get_disk_buffer_mb();
	string getCpuUsage(bool writeThread = false, bool preparePstatData = false);
	bool socketWritePcapBlock(pcap_block_store *blockStore);
	bool socketGetHost();
	bool socketReadyForConnect();
	bool socketConnect();
	bool socketListen();
	bool socketAwaitConnection(int *socketClient, sockaddr_in *socketClientInfo);
	bool socketClose();
	bool socketWrite(u_char *data, size_t dataLen, bool disableAutoConnect = false);
	bool _socketWrite(int socket, u_char *data, size_t *dataLen, int timeout = 1);
	bool socketRead(u_char *data, size_t *dataLen, int idConnection);
	bool _socketRead(int socket, u_char *data, size_t *dataLen, int timeout = 1);
	bool isMirrorSender() {
		return(this->packetServerDirection == directionWrite);
	}
	bool isMirrorReceiver() {
		return(this->packetServerDirection == directionRead);
	}
private:
	void createConnection(int socketClient, sockaddr_in *socketClientInfo);
	void cleanupConnections(bool all = false);
	void processPacket(pcap_pkthdr_plus *header, u_char *packet,
			   pcap_block_store *block_store, int block_store_index,
			   int dlt, int sensor_id);
	void pushBatchProcessPacket();
	void checkFreeSizeCachedir();
	void cleanupBlockStoreTrash(bool all = false);
	void lock_packetServerConnections() {
		while(__sync_lock_test_and_set(&this->_sync_packetServerConnections, 1));
	}
	void unlock_packetServerConnections() {
		__sync_lock_release(&this->_sync_packetServerConnections);
	}
protected:
	ip_port packetServerIpPort;
	ePacketServerDirection packetServerDirection;
	pcap_t *fifoReadPcapHandle;
	pcap_t *pcapDeadHandles[DLT_TYPES_MAX];
	int pcapDeadHandles_dlt[DLT_TYPES_MAX];
	int pcapDeadHandles_count;
	pthread_t socketServerThreadHandle;
private:
	pcap_store_queue pcapStoreQueue;
	deque<pcap_block_store*> blockStoreTrash;
	u_int cleanupBlockStoreTrash_counter;
	u_int32_t socketHostIPl;
	int socketHandle;
	map<unsigned int, sPacketServerConnection*> packetServerConnections;
	volatile int _sync_packetServerConnections;
	u_long lastCheckFreeSizeCachedir_timeMS;
	timeval _last_ts;
friend void *_PcapQueue_readFromFifo_socketServerThreadFunction(void *arg);
friend void *_PcapQueue_readFromFifo_connectionThreadFunction(void *arg);
};


void PcapQueue_init();
void PcapQueue_term();
int getThreadingMode();


#endif
