
#ifndef DEF_GRAPHCHWALKER_ENGINE
#define DEF_GRAPHCHWALKER_ENGINE

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <omp.h>
#include <vector>
#include <map>
#include <sys/time.h>

#include "api/filename.hpp"
#include "api/io.hpp"
#include "logger/logger.hpp"
#include "metrics/metrics.hpp"
#include "api/pthread_tools.hpp"
#include "walks/randomwalk.hpp"

class graphwalker_engine {
public:     
    std::string base_filename;
    // unsigned membudget_mb;
    unsigned long long blocksize_kb;  
    bid_t nblocks;  
    vid_t nvertices;      
    tid_t exec_threads;
    vid_t* blocks;
    timeval start;
    
    /* Ｉn memory blocks */
    bid_t nmblocks; //number of in memory blocks
    vid_t **csrbuf;
    eid_t **beg_posbuf;
    bid_t cmblocks; //current number of in memory blocks
    bid_t *inMemIndex;

    /* State */
    bid_t exec_block;
    
    /* Metrics */
    metrics &m;
    WalkManager *walk_manager;
        
    void print_config() {
        logstream(LOG_INFO) << "Engine configuration: " << std::endl;
        logstream(LOG_INFO) << " exec_threads = " << (int)exec_threads << std::endl;
        logstream(LOG_INFO) << " blocksize_kb = " << blocksize_kb << "kb" << std::endl;
        logstream(LOG_INFO) << " number of total blocks = " << nblocks << std::endl;
        logstream(LOG_INFO) << " number of in-memory blocks = " << nmblocks << std::endl;
    }

    double runtime() {
            timeval end;
            gettimeofday(&end, NULL);
            return end.tv_sec-start.tv_sec+ ((double)(end.tv_usec-start.tv_usec))/1.0E6;
        }
        
public:
        
    /**
     * Initialize GraphChi engine
     * @param base_filename prefix of the graph files
     * @param nblocks number of shards
     * @param selective_scheduling if true, uses selective scheduling 
     */
    graphwalker_engine(std::string _base_filename, unsigned long long _blocksize_kb, bid_t _nblocks, bid_t _nmblocks, metrics &_m) : base_filename(_base_filename), blocksize_kb(_blocksize_kb), nblocks(_nblocks), nmblocks(_nmblocks), m(_m) {
        // membudget_mb = get_option_int("membudget_mb", 1024);
        exec_threads = get_option_int("execthreads", omp_get_max_threads());
        omp_set_num_threads(exec_threads);
        load_block_range(base_filename, blocksize_kb, blocks);
        nvertices = num_vertices();
        walk_manager = new WalkManager(m,nblocks,exec_threads,base_filename);

        csrbuf = (vid_t**)malloc(nmblocks*sizeof(vid_t*));
        for(bid_t b = 0; b < nmblocks; b++)
            csrbuf[b] = (vid_t*)malloc(blocksize_kb*1024);
        beg_posbuf = (eid_t**)malloc(nmblocks*sizeof(eid_t*));
        inMemIndex = (bid_t*)malloc(nblocks*sizeof(bid_t));
        for(bid_t b = 0; b < nblocks; b++)  inMemIndex[b] = nmblocks;
        cmblocks = 0;

        _m.set("file", _base_filename);
        _m.set("engine", "default");
        _m.set("nblocks", (size_t)nblocks);

        print_config();
    }
        
    virtual ~graphwalker_engine() {
        delete walk_manager;
        for(bid_t b = 0; b < nmblocks; b++){
            if(beg_posbuf[b] != NULL)   free(beg_posbuf[b]);
            if(csrbuf[b] != NULL)   free(csrbuf[b]);
        }
        free(beg_posbuf);
        free(csrbuf);
        free(inMemIndex);
    }

    void load_block_range(std::string base_filename, unsigned long long blocksize_kb, vid_t * &blocks, bool allowfail=false) {
        std::string blockrangefile = blockrangename(base_filename, blocksize_kb);
        std::ifstream brf(blockrangefile.c_str());
        
        if (!brf.good()) {
            logstream(LOG_ERROR) << "Could not load block range file: " << blockrangefile << std::endl;
        }
        assert(brf.good());
        
        blocks = (vid_t*)malloc((nblocks+1)*sizeof(vid_t));
        vid_t en;
        for(bid_t i=0; i < nblocks+1; i++) {
            assert(!brf.eof());
            brf >> en;
            blocks[i] = en;
        }
        for(bid_t i=nblocks-1; i < nblocks; i++) {
             logstream(LOG_INFO) << "last shard: " << blocks[i] << " - " << blocks[i+1] << std::endl;
        }
        brf.close();
    }

    void loadSubGraph(bid_t p, eid_t * &beg_pos, vid_t * &csr, vid_t *nverts, eid_t *nedges){
        m.start_time("loadSubGraph");
        std::string invlname = fidname( base_filename, 0 ); //only 1 file
        std::string beg_posname = invlname + ".beg_pos";
        std::string csrname = invlname + ".csr";
        int beg_posf = open(beg_posname.c_str(),O_RDONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
        int csrf = open(csrname.c_str(),O_RDONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
        if (csrf < 0 || beg_posf < 0) {
            logstream(LOG_FATAL) << "Could not load :" << csrname << " or " << beg_posname << ", error: " << strerror(errno) << std::endl;
        }
        assert(csrf > 0 && beg_posf > 0);

        /* read beg_pos file */
        *nverts = blocks[p+1] - blocks[p];
        // logstream(LOG_INFO) << "*nverts : "<< *nverts << ", blocks[p] : "<< blocks[p] << std::endl;
        beg_pos = (eid_t*) malloc((*nverts+1)*sizeof(eid_t));
        preada(beg_posf, beg_pos, (size_t)(*nverts+1)*sizeof(eid_t), (size_t)blocks[p]*sizeof(eid_t));        
        close(beg_posf);
        /* read csr file */
        *nedges = beg_pos[*nverts] - beg_pos[0];
        //csr = (vid_t*) malloc((*nedges)*sizeof(vid_t));
        preada(csrf, csr, (*nedges)*sizeof(vid_t), beg_pos[0]*sizeof(vid_t));
        close(csrf);       

        /*output load graph info*/
        // logstream(LOG_INFO) << "LoadSubGraph data end, with nverts = " << *nverts << ", " << "nedges = " << *nedges << std::endl;
        
        // logstream(LOG_INFO) << "beg_pos : "<< std::endl;
        // for(vid_t i = *nverts-10; i < *nverts; i++)
        //     logstream(LOG_INFO) << "beg_pos[" << i << "] = " << beg_pos[i] << ", "<< std::endl;
        // logstream(LOG_INFO) << "csr : "<< std::endl;
        // for(eid_t i = *nedges-10; i < *nedges; i++)
        //     logstream(LOG_INFO) << "csr[" << i << "] = " << csr[i] << ", "<< std::endl;
        m.stop_time("loadSubGraph");
    }

    void findSubGraph(bid_t p, eid_t * &beg_pos, vid_t * &csr, vid_t *nverts, eid_t *nedges){
        if(inMemIndex[p] == nmblocks){//the block is not in memory
            // logstream(LOG_INFO) << "Load block " << p << " from disk" << std::endl;
            bid_t swapin;
            if(cmblocks < nmblocks){
                swapin = cmblocks++;
            }else{
                swapin = swapOut();
                assert(swapin < nmblocks);
                if(beg_posbuf[swapin] != NULL) free(beg_posbuf[swapin]);
            }
            loadSubGraph(p, beg_posbuf[swapin], csrbuf[swapin], nverts, nedges);
            inMemIndex[p] = swapin;
        }else{
            // logstream(LOG_INFO) << "Oh yeah! Block " << p << " is in memory!" << std::endl;
        }
        beg_pos = beg_posbuf[ inMemIndex[p] ];
        csr = csrbuf[ inMemIndex[p] ];
    }

    bid_t swapOut(){
        wid_t minmw = walk_manager->walksum();
        bid_t minmwb = 0;
        for(bid_t b = 0; b < nblocks; b++){
            if(inMemIndex[b]<nmblocks && walk_manager->walknum[b] < minmw){
                minmw = walk_manager->walknum[b];
                minmwb = b;
            }
        }
        // logstream(LOG_DEBUG) << "block " << minmwb << " is chosen to swap out!" << std::endl;
        bid_t res = inMemIndex[minmwb];
        inMemIndex[minmwb] = nmblocks;
        return res;
    }

    virtual size_t num_vertices() {
        return blocks[nblocks];
    }

    void exec_updates(RandomWalk &userprogram, eid_t *&beg_pos, vid_t *&csr){ //, VertexDataType* vertex_value){
        // unsigned count = walk_manager->readblockWalks(exec_block);
        m.start_time("exec_updates");
        // logstream(LOG_INFO) << "exec_updates.." << std::endl;
        omp_set_num_threads(exec_threads);
        for(tid_t t = 0; t < exec_threads; t++){
            wid_t walkcount = walk_manager->pwalks[t][exec_block].size();
            #pragma omp parallel for schedule(static)
                for(wid_t i = 0; i < walkcount; i++ ){
                    // logstream(LOG_INFO) << "exec_block : " << exec_block << " , walk : " << i << " --> threads." << omp_get_thread_num() << std::endl;
                    WalkDataType walk = walk_manager->pwalks[t][exec_block][i];
                    userprogram.updateByWalk(walk, i, exec_block, beg_pos, csr, *walk_manager );//, vertex_value);
                }
        }
        // logstream(LOG_INFO) << "exec_updates end. Processsed walks with exec_threads = " << (int)exec_threads << std::endl;
        m.stop_time("exec_updates");
        // walk_manager->writeblockWalks(exec_block);
    }

    void run(RandomWalk &userprogram, float prob) {
        gettimeofday(&start, NULL);
        // srand((unsigned)time(NULL));
        m.start_time("startWalks");
        userprogram.startWalks(*walk_manager, nblocks, blocks);
        m.stop_time("startWalks");

        m.start_time("runtime");
        vid_t nverts, *csr;
        eid_t nedges, *beg_pos;
        /*loadOnDemand -- block loop */
        int blockcount = 0;
        while( userprogram.hasFinishedWalk(*walk_manager) ){
            m.start_time("in_run_block");
            blockcount++;
            exec_block = walk_manager->chooseBlock(prob);
            findSubGraph(exec_block, beg_pos, csr, &nverts, &nedges);

            /*load walks info*/
            // walk_manager->loadWalkPool(exec_block);
            wid_t nwalks; 
            nwalks = userprogram.before_exec_block(exec_block, blocks[exec_block], blocks[exec_block+1], *walk_manager);
            
            if(blockcount % 100==1){
                logstream(LOG_DEBUG) << runtime() << "s : blockcount: " << blockcount << " : " << exec_block << std::endl;
                logstream(LOG_INFO) << "nverts = " << nverts << ", nedges = " << nedges << std::endl;
                logstream(LOG_INFO) << "walksum = " << walk_manager->walksum() << ", nwalks[p] = " << nwalks << std::endl;
            }
            
            exec_updates(userprogram, beg_pos, csr);
            userprogram.after_exec_block(exec_block, blocks[exec_block], blocks[exec_block+1], *walk_manager);
            // walk_manager->walknum[exec_block] = 0;
            // walk_manager->writeWalkPools();

            m.stop_time("in_run_block");
        } // For block loop
        m.stop_time("runtime");
    }
};

#endif