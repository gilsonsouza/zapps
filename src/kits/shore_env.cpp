/* -*- mode:C++; c-basic-offset:4 -*-
     Shore-kits -- Benchmark implementations for Shore-MT

                       Copyright (c) 2007-2009
      Data Intensive Applications and Systems Labaratory (DIAS)
               Ecole Polytechnique Federale de Lausanne

                         All Rights Reserved.

   Permission to use, copy, modify and distribute this software and
   its documentation is hereby granted, provided that both the
   copyright notice and this permission notice appear in all copies of
   the software, derivative works or modified versions, and any
   portions thereof, and that both notices appear in supporting
   documentation.

   This code is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
   DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
   RESULTING FROM THE USE OF THIS SOFTWARE.
*/

/** @file shore_env.cpp
 *
 *  @brief Implementation of a Shore environment (database)
 *
 *  @author Ippokratis Pandis (ipandis)
 */


// #include "util/confparser.h"
#include "shore_env.h"
#include "envvar.h"
#include "trx_worker.h"
#include "daemons.h"
#include "util/random_input.h"
// #include "sm/shore/shore_flusher.h"
// #include "sm/shore/shore_helper_loader.h"


// #warning IP: TODO pass arbitrary -sm_* options from shore.conf to shore

// Exported variables //

ShoreEnv* _g_shore_env = NULL;

// procmonitor_t* _g_mon = NULL;



// Exported functions //



/********************************************************************
 *
 *  @fn:    print_env_stats
 *
 *  @brief: Prints trx statistics
 *
 ********************************************************************/

void env_stats_t::print_env_stats() const
{
    TRACE( TRACE_STATISTICS, "===============================\n");
    TRACE( TRACE_STATISTICS, "Database transaction statistics\n");
    TRACE( TRACE_STATISTICS, "Attempted: %d\n", _ntrx_att);
    TRACE( TRACE_STATISTICS, "Committed: %d\n", _ntrx_com);
    TRACE( TRACE_STATISTICS, "Aborted  : %d\n", (_ntrx_att-_ntrx_com));
    TRACE( TRACE_STATISTICS, "===============================\n");
}




/********************************************************************
 *
 *  @class: ShoreEnv
 *
 *  @brief: The base class for all the environments (== databases)
 *          in Shore-MT
 *
 ********************************************************************/

ShoreEnv::ShoreEnv()
    : db_iface(),
      _pssm(NULL),
      _initialized(false), _init_mutex(thread_mutex_create()),
      _loaded(false), _load_mutex(thread_mutex_create()),
      _statmap_mutex(thread_mutex_create()),
      _last_stats_mutex(thread_mutex_create()),
      _vol_mutex(thread_mutex_create()),
      _max_cpu_count(0),
      _active_cpu_count(0),
      _worker_cnt(0),
      _measure(MST_UNDEF),
      _pd(PD_NORMAL),
      _insert_freq(0),_delete_freq(0),_probe_freq(100),
      _chkpt_freq(0),
      _enable_archiver(false), _enable_merger(false),
      _activation_delay(0),
      _crash_delay(0),
      _bAlarmSet(false),
      _start_imbalance(0),
      _skew_type(SKEW_NONE),
      _request_pool(sizeof(trx_request_t)),
      _bUseSLI(false),
      _bUseELR(false),
      _bUseFlusher(false)
      // _logger(NULL)
{
    _popts = new sm_options();
    _vid = vid_t(1);

    pthread_mutex_init(&_scaling_mutex, NULL);
    pthread_mutex_init(&_queried_mutex, NULL);

    // Read configuration
    envVar* ev = envVar::instance();

    string physical = ev->getSysDesign();

    if (physical.compare("normal")==0) {
        _pd = PD_NORMAL;
    }

    // Check about the hacks option
    check_hacks_enabled();
    if (is_hacks_enabled()) {
        _pd |= PD_PADDED;
    }


    _bUseSLI = ev->getVarInt("db-worker-sli",0);
    fprintf(stdout, "SLI= %s\n", (_bUseSLI ? "enabled" : "disabled"));

    // Used by some benchmarks
    _rec_to_acc = ev->getVarInt("records-to-access",1);
}


ShoreEnv::~ShoreEnv()
{
    if (dbc()!=DBC_STOPPED) stop();

    pthread_mutex_destroy(&_init_mutex);
    pthread_mutex_destroy(&_statmap_mutex);
    pthread_mutex_destroy(&_last_stats_mutex);
    pthread_mutex_destroy(&_load_mutex);
    pthread_mutex_destroy(&_vol_mutex);

    pthread_mutex_destroy(&_scaling_mutex);
    pthread_mutex_destroy(&_queried_mutex);
}


bool ShoreEnv::is_initialized()
{
    CRITICAL_SECTION(cs, _init_mutex);
    return (_initialized);
}

bool ShoreEnv::is_loaded()
{
    CRITICAL_SECTION(cs, _load_mutex);
    return (_loaded);
}

w_rc_t ShoreEnv::load()
{
    // 1. lock the loading status and the scaling factor
    CRITICAL_SECTION(load_cs, _load_mutex);
    if (_loaded) {
        // TRACE( TRACE_TRX_FLOW,
        //       "Env already loaded. Doing nothing...\n");
        return (RCOK);
    }
    CRITICAL_SECTION(scale_cs, _scaling_mutex);
    time_t tstart = time(NULL);

    // 2. Invoke benchmark-specific table creator
    W_DO(create_tables());

    // 3. Kick-off checkpoint thread
    // guard<checkpointer_t> chk(new checkpointer_t(this));
    // if (_chkpt_freq > 0) {
    //     chk->fork();
    // }

    // CS: loaders only start working once this is set (was in old checkpointer code)
    set_measure(MST_MEASURE);

    // 4. Invoke benchmark-specific table loaders
    W_DO(load_data());

    set_measure(MST_PAUSE);

    // 5. Print stats, join checkpointer, and return
    time_t tstop = time(NULL);
    TRACE( TRACE_ALWAYS, "Loading finished in (%d) secs...\n", (tstop - tstart));

    // if (_chkpt_freq > 0) {
    // 	chk->set_active(false);
    // 	chk->join();
    // }

    _loaded = true;
    return RCOK;
}

void ShoreEnv::set_max_cpu_count(const uint cpucnt)
{
    _max_cpu_count = ( cpucnt>0 ? cpucnt : _max_cpu_count);
    if (_active_cpu_count>_max_cpu_count) {
        _active_cpu_count = _max_cpu_count;
    }
}

uint ShoreEnv::get_max_cpu_count() const
{
    return (_max_cpu_count);
}

uint ShoreEnv::get_active_cpu_count() const
{
    return (_active_cpu_count);
}


/********************************************************************
 *
 *  @fn:    Related to Scaling and querying factor
 *
 *  @brief: Scaling factor (sf) - The size of the entire database
 *          Queried factor (qf) - The part of the database accessed at a run
 *
 ********************************************************************/

void ShoreEnv::set_qf(const double aQF)
{
    if ((aQF>0) && (aQF<=_scaling_factor)) {
        TRACE( TRACE_ALWAYS, "New Queried Factor: %.1f\n", aQF);
        _queried_factor = aQF;
    }
    else {
        TRACE( TRACE_ALWAYS, "Invalid queried factor input: %.1f\n", aQF);
    }
}

double ShoreEnv::get_qf() const
{
    return (_queried_factor);
}


void ShoreEnv::set_sf(const double aSF)
{
    if (aSF > 0.0) {
        TRACE( TRACE_ALWAYS, "New Scaling factor: %.1f\n", aSF);
        _scaling_factor = aSF;
    }
    else {
        TRACE( TRACE_ALWAYS, "Invalid scaling factor input: %.1f\n", aSF);
    }
}

double ShoreEnv::get_sf() const
{
    return (_scaling_factor);
}

void ShoreEnv::print_sf() const
{
    TRACE( TRACE_ALWAYS, "Scaling Factor = (%.1f)\n", get_sf());
    TRACE( TRACE_ALWAYS, "Queried Factor = (%.1f)\n", get_qf());
}

// void ShoreEnv::log_insert(kits_logger_t::logrec_kind_t kind)
// {
//     rc_t rc = _logger->insert(kind);
//     if (rc.is_error()) {
//     	TRACE( TRACE_ALWAYS, "!! Error inserting kits log record: %s\n",
//     			w_error_t::error_string(rc.err_num()));
//     }
// }


/********************************************************************
 *
 *  @fn:    Related to physical design
 *
 ********************************************************************/

uint4_t ShoreEnv::get_pd() const
{
    return (_pd);
}

uint4_t ShoreEnv::set_pd(const physical_design_t& apd)
{
    _pd = apd;
    TRACE( TRACE_ALWAYS, "DB set to (%x)\n", _pd);
    return (_pd);
}

uint4_t ShoreEnv::add_pd(const physical_design_t& apd)
{
    _pd |= apd;
    TRACE( TRACE_ALWAYS, "DB set to (%x)\n", _pd);
    return (_pd);
}

bool ShoreEnv::check_hacks_enabled()
{
    // enable hachs by default
    int he = envVar::instance()->getVarInt("physical-hacks-enable",0);
    _enable_hacks = (he == 1 ? true : false);
    return (_enable_hacks);
}

bool ShoreEnv::is_hacks_enabled() const
{
    return (_enable_hacks);
}


/********************************************************************
 *
 *  @fn:    Related to microbenchmarks
 *  @brief: Set the insert/delete/probe frequencies
 *
 ********************************************************************/
void ShoreEnv::set_freqs(int insert_freq, int delete_freq, int probe_freq)
{
    assert ((insert_freq>=0) && (insert_freq<=100));
    assert ((delete_freq>=0) && (delete_freq<=100));
    assert ((probe_freq>=0) && (probe_freq<=100));
    _insert_freq = insert_freq;
    _delete_freq = delete_freq;
    _probe_freq = probe_freq;
}

void ShoreEnv::set_chkpt_freq(int chkpt_freq)
{
    _chkpt_freq = chkpt_freq;
}

void ShoreEnv::set_archiver_opts(bool enable_archiver, bool enable_merger)
{
    _enable_archiver = enable_archiver;
    _enable_merger = enable_merger;
}

void ShoreEnv::set_crash_delay(int crash_delay)
{
    _crash_delay = crash_delay;
}

/********************************************************************
 *
 *  @fn:    Related to load balancing work
 *  @brief: Set the load imbalance and the time to start it
 *
 ********************************************************************/
void ShoreEnv::set_skew(int area, int load, int start_imbalance, int skew_type)
{
    // CS TODO -- not used
    (void) area;
    (void) load;
    assert ((load>=0) && (load<=100));
    assert (start_imbalance>0);
    assert((area>0) && (area<100));

    _start_imbalance = start_imbalance;

    if (skew_type <= 0)
        skew_type = URand(1,10);
    if(skew_type < 6) {
	// 1. Keep the initial skew (no changes)
	_skew_type = SKEW_NORMAL;
	TRACE( TRACE_ALWAYS, "SKEW_NORMAL\n");
    } else if(skew_type < 9) {
	// 2. Change the area of the initial skew after some random duration
	_skew_type = SKEW_DYNAMIC;
	TRACE( TRACE_ALWAYS, "SKEW_DYNAMIC\n");
    } else if(skew_type < 11) {
	// 3. Change the initial skew randomly after some random duration
	// (a) The new skew can be like the old one but in another spot
	// (b) The skew can be omitted for sometime and
	// (c) The percentages might be changed
	_skew_type = SKEW_CHAOTIC;
	TRACE( TRACE_ALWAYS, "SKEW_CHAOTIC\n");
    } else {
	assert(0); // More cases can be added as wanted
    }
}


/********************************************************************
 *
 *  @fn:    Related to load balancing work
 *  @brief: reset the load imbalance and the time to start it if necessary
 *
 ********************************************************************/
void ShoreEnv::start_load_imbalance()
{
    // @note: pin: can change these boundaries depending on preference
    if(_skew_type == SKEW_DYNAMIC || _skew_type == SKEW_CHAOTIC) {
	_start_imbalance = URand(10,30);
	_bAlarmSet = false;
    }
}


/********************************************************************
 *
 *  @fn:    Related to load balancing work
 *  @brief: Set the flags to stop the load imbalance
 *
 ********************************************************************/
void ShoreEnv::reset_skew()
{
    _start_imbalance = 0;
    _bAlarmSet = false;
}


/********************************************************************
 *
 *  @fn:    Related to environment workers
 *
 *  @brief: Each environment has a set of worker threads
 *
 ********************************************************************/

uint ShoreEnv::upd_worker_cnt()
{
    // update worker thread cnt
    uint workers = envVar::instance()->getVarInt("db-workers",0);
    assert (workers);
    _worker_cnt = workers;
    return (_worker_cnt);
}


trx_worker_t* ShoreEnv::worker(const uint idx)
{
    return (_workers[idx%_worker_cnt]);
}




/********
 ******** Caution: The functions below should be invoked inside
 ********          the context of a smthread
 ********/


/*********************************************************************
 *
 *  @fn     init
 *
 *  @brief  Initialize the shore environment. Reads configuration
 *          opens database and device.
 *
 *  @return 0 on success, non-zero otherwise
 *
 *********************************************************************/

int ShoreEnv::init()
{
    CRITICAL_SECTION(cs,_init_mutex);
    if (_initialized) {
        TRACE( TRACE_ALWAYS, "Already initialized\n");
        return (0);
    }

    // Read configuration options
    // We do not pass the configuration file name anymore.
    // Instead, this should have been setup at envVar (the global environment)
    readconfig();

    // Set sys params
    if (_set_sys_params()) {
        TRACE( TRACE_ALWAYS, "Problem in setting system parameters\n");
        return (1);
    }


    // Apply configuration to the storage manager
    if (configure_sm()) {
        TRACE( TRACE_ALWAYS, "Error configuring Shore\n");
        return (2);
    }

    // Load the database schema
    if (load_schema().is_error()) {
        TRACE( TRACE_ALWAYS, "Error loading the database schema\n");
        return (3);
    }

    // Update partitioning information
    if (update_partitioning().is_error()) {
        TRACE( TRACE_ALWAYS, "Error updating the partitioning info\n");
        return (4);
    }


    return (0);
}


/*********************************************************************
 *
 *  @fn:     start
 *
 *  @brief:  Starts the Shore environment
 *           Updates the scaling/queried factors and starts the workers
 *
 *  @return: 0 on success, non-zero otherwise
 *
 *********************************************************************/

int ShoreEnv::start()
{
    // Start the storage manager
    if (start_sm()) {
        TRACE( TRACE_ALWAYS, "Error starting Shore database\n");
        return (5);
    }

    if (!_clobber) {
	// Cache fids at the kits side
	W_COERCE(db()->begin_xct());
	W_COERCE(load_and_register_fids());
	W_COERCE(db()->commit_xct());
	// Call the (virtual) post-initialization function
        if (int rval = post_init()) {
            TRACE( TRACE_ALWAYS, "Error in Shore post-init\n");
            return (rval);
        }
    }

    // if we reached this point the environment is properly initialized
    _initialized = true;
    TRACE( TRACE_DEBUG, "ShoreEnv initialized\n");

    upd_worker_cnt();

    assert (_workers.empty());

    TRACE( TRACE_ALWAYS, "Starting (%s)\n", _sysname.c_str());
    info();

    // read from env params the loopcnt
    int lc = envVar::instance()->getVarInt("db-worker-queueloops",0);

#ifdef CFG_FLUSHER
    _start_flusher();
#endif

    WorkerPtr aworker;
    for (uint i=0; i<_worker_cnt; i++) {
        aworker = new Worker(this,std::string("work-%d", i), -1,_bUseSLI);
        _workers.push_back(aworker);
        aworker->init(lc);
        aworker->start();
        aworker->fork();
    }
    return (0);
}



/*********************************************************************
 *
 *  @fn:     stop
 *
 *  @brief:  Stops the Shore environment
 *
 *  @return: 0 on success, non-zero otherwise
 *
 *********************************************************************/

int ShoreEnv::stop()
{
    // Check if initialized
    CRITICAL_SECTION(cs, _init_mutex);

    if (dbc() == DBC_STOPPED)
    {
        // Already stopped
        TRACE( TRACE_ALWAYS, "(%s) already stopped\n",
               _sysname.c_str());
        return (0);
    }

    TRACE( TRACE_ALWAYS, "Stopping (%s)\n", _sysname.c_str());
    info();

    if (!_initialized) {
        cerr << "Environment not initialized..." << endl;
        return (1);
    }

    // Stop workers
    int i=0;
    for (WorkerIt it = _workers.begin(); it != _workers.end(); ++it) {
        i++;
        TRACE( TRACE_DEBUG, "Stopping worker (%d)\n", i);
        if (*it) {
            (*it)->stop();
            (*it)->join();
            delete (*it);
        }
    }
    _workers.clear();

#ifdef CFG_FLUSHER
    _stop_flusher();
#endif

    // Set the stoped flag
    set_dbc(DBC_STOPPED);

    // If reached this point the Shore environment is stopped
    return (0);
}



/*********************************************************************
 *
 *  @fn:     close
 *
 *  @brief:  Closes the Shore environment
 *
 *  @return: 0 on success, non-zero otherwise
 *
 *********************************************************************/

int ShoreEnv::close()
{
    TRACE( TRACE_ALWAYS, "Closing (%s)\n", _sysname.c_str());

    // First stop the environment
    int r = stop();
    if (r != 0)
    {
        // If it returned != 0 then error occured
        return (r);
    }

    // Then, close Shore-MT
    CRITICAL_SECTION(cs, _init_mutex);
    close_sm();
    _initialized = false;

    // If reached this point the Shore environment is closed
    return (0);
}


/********************************************************************
 *
 *  @fn:    statistics
 *
 *  @brief: Prints statistics for the SM
 *
 ********************************************************************/

int ShoreEnv::statistics()
{
    CRITICAL_SECTION(cs, _init_mutex);
    if (!_initialized) {
        cerr << "Environment not initialized..." << endl;
        return (1);
    }

#ifdef CFG_FLUSHER
    if (_base_flusher) _base_flusher->statistics();
#endif

    // If reached this point the Shore environment is closed
    //gatherstats_sm();
    return (0);
}



/** Storage manager functions */


/********************************************************************
 *
 *  @fn:     close_sm
 *
 *  @brief:  Closes the storage manager
 *
 *  @return: 0 on sucess, non-zero otherwise
 *
 ********************************************************************/

int ShoreEnv::close_sm()
{
    TRACE( TRACE_ALWAYS, "Closing Shore storage manager...\n");

    if (!_pssm) {
        TRACE( TRACE_ALWAYS, "sm already closed...\n");
        return (1);
    }


    // Disabling fake io delay, if any
    _pssm->disable_fake_disk_latency(_vid);

    // check if any active xcts
    int activexcts = ss_m::num_active_xcts();
    if (activexcts) {
        TRACE (TRACE_ALWAYS, "\n*** Warning (%d) active xcts. Cannot dismount!!\n",
               activexcts);

        W_IGNORE(ss_m::dump_xcts(cout));
        cout << flush;
    }

    // Final stats
    gatherstats_sm();

    /*
     *  destroying the ss_m instance causes the SSM to shutdown
     */
    delete (_pssm);

    // If we reached this point the sm is closed
    return (0);
}


/********************************************************************
 *
 *  @fn:     gatherstats_sm
 *
 *  @brief:  Collects and prints statistics from the sm
 *
 ********************************************************************/

static sm_stats_info_t oldstats;

void ShoreEnv::gatherstats_sm()
{
    // sm_du_stats_t stats;
    // memset(&stats, 0, sizeof(stats));

    sm_stats_info_t stats;
    ss_m::gather_stats(stats);

    sm_stats_info_t diff = stats;
    diff -= _last_sm_stats;

    // Print the diff and save the latest reading
    cout << diff << endl;
    _last_sm_stats = stats;
}



/********************************************************************
 *
 *  @fn:     configure_sm
 *
 *  @brief:  Configure Shore environment
 *
 *  @return: 0 on sucess, non-zero otherwise
 *
 ********************************************************************/

int ShoreEnv::configure_sm()
{
    TRACE( TRACE_DEBUG, "Configuring Shore...\n");

    for (map<string,string>::iterator sm_iter = _sm_opts.begin();
         sm_iter != _sm_opts.end(); sm_iter++)
    {
        // the "+1" is a hack to remove the preceding "-" from options
        // e.g., -sm_logsize -> sm_logsize
        string key = sm_iter->first.c_str() + 1;
        string value = sm_iter->second.c_str();

        // In Zero, we have to call the specific method depending on the
        // type of the option, so we attempt casts to determine that
        if (value.compare("yes") == 0 || value.compare("no") == 0)
        {
            // boolean options are set with yes/no in shore.conf
            // TODO: is true/false or 1/0 allowed?
            bool bvalue = (value.compare("yes") == 0);
            _popts->set_bool_option(key, bvalue);
        }
        else {
            std::istringstream iss(value);
            int ivalue = 0;

            if (!(iss >> ivalue).fail()) {
                // if conversion succeeds, option is integer
                _popts->set_int_option(key, ivalue);
            }
            else {
                // otherwise it must be string
                _popts->set_string_option(key, value);
            }
        }
    }

    upd_worker_cnt();

    // If we reached this point the sm is configured correctly
    return (0);
}



/******************************************************************
 *
 *  @fn:     start_sm()
 *
 *  @brief:  Start Shore storage manager.
 *           - Format and mount the device
 *           - Create the volume in the device
 *           (Shore limitation: only 1 volume per device)
 *           - Set the fakeiodelay params
 *
 *  @return: 0 on success, non-zero otherwise
 *
 ******************************************************************/
int /*shore::*/ssm_max_small_rec;
sm_config_info_t  sm_config_info;

int ShoreEnv::start_sm()
{
    TRACE( TRACE_DEBUG, "Starting Shore...\n");

    if (_initialized == false) {
        _pssm = new ss_m(*_popts);
        // _logger = new kits_logger_t(_pssm);
    }
    else {
        TRACE( TRACE_DEBUG, "Shore already started...\n");
        return (1);
    }

    // format and mount the database...

    assert (_pssm);
    assert (!_device.empty());
    assert (_quota>0);

    if (_clobber) {
        // if didn't clobber then the db is already loaded
        CRITICAL_SECTION(cs, _load_mutex);

        TRACE( TRACE_DEBUG, "Formatting a new device (%s) with a (%d) kB quota\n",
               _device.c_str(), _quota);

	// create and mount device
	// http://www.cs.wisc.edu/shore/1.0/man/device.ssm.html
        ss_m::smksize_t smquota = _quota;
	W_COERCE(_pssm->create_vol(_device.c_str(), smquota, _vid));
        TRACE( TRACE_DEBUG, "Formatting device completed...\n");

        // mount it...
        W_COERCE(_pssm->mount_vol(_device.c_str(), _vid));
        TRACE( TRACE_DEBUG, "Mounting (new) device completed...\n");

        // create catalog index (must be on stid 1)
        stid_t cat_stid;
        W_COERCE(_pssm->begin_xct());
        W_COERCE(_pssm->create_index(_vid, cat_stid));
        w_assert0(cat_stid == stid_t(_vid, 1));
        W_COERCE(_pssm->commit_xct());

        // set that the database is not loaded
        _loaded = false;
    }
    else {
        // if didn't clobber then the db is already loaded
        CRITICAL_SECTION(cs, _load_mutex);

        TRACE( TRACE_DEBUG, "Using device (%s)\n", _device.c_str());

        // CS: No need to mount since mounted devices are restored during
        // log analysis, i.e., list of mounted devices is kept in the persistent
        // system state. Mount here is only necessary if we explicitly dismount
        // after loading, which is not the case.

        // Make sure that catalog index (stid 1) exists
        vol_t* vol = ss_m::vol->get(_vid);
        w_assert0(vol);
        w_assert0(vol->is_alloc_store(1));
        w_assert0(strcmp(vol->devname(), _device.c_str()) == 0);

        // "speculate" that the database is loaded
        _loaded = true;
    }

    // setting the fake io disk latency - after we mount
    // (let the volume be formatted and mounted without any fake io latency)
    envVar* ev = envVar::instance();
    int enableFakeIO = ev->getVarInt("shore-fakeiodelay-enable",0);
    TRACE( TRACE_DEBUG, "Is fake I/O delay enabled: (%d)\n", enableFakeIO);
    if (enableFakeIO) {
        _pssm->enable_fake_disk_latency(_vid);
    }
    else {
        _pssm->disable_fake_disk_latency(_vid);
    }
    int ioLatency = ev->getVarInt("shore-fakeiodelay",0);
    TRACE( TRACE_DEBUG, "I/O delay latency set: (%d)\n", ioLatency);
    W_COERCE(_pssm->set_fake_disk_latency(_vid,ioLatency));

    // Using the physical ID interface

    // Get the configuration info so we have the max size of a small record
    // for use in _post_init_impl()
    if (ss_m::config_info(sm_config_info).is_error()) return (1);
    ssm_max_small_rec = sm_config_info.max_small_rec;


    // If we reached this point the sm has started correctly
    return (0);
}



/*********************************************************************
 *
 *  @fn:      checkpoint()
 *
 *  @brief:   Takes a db checkpoint
 *
 *  @note:    Used between iterations and measurements that explicitly
 *            request periodic checkpoints.
 *
 *********************************************************************/

int ShoreEnv::checkpoint()
{
    _pssm->checkpoint();
    return 0;
}

void ShoreEnv::activate_archiver()
{
    if (_enable_archiver) {
        _pssm->activate_archiver();
    }
}

void ShoreEnv::activate_merger()
{
    if (_enable_merger) {
        _pssm->activate_merger();
    }
}

/******************************************************************
 *
 *  @fn:    get_trx_{att,com}()
 *
 ******************************************************************/

unsigned ShoreEnv::get_trx_att() const
{
    return (*&_env_stats._ntrx_att);
}

unsigned ShoreEnv::get_trx_com() const
{
    return (*&_env_stats._ntrx_com);
}



/******************************************************************
 *
 *  @fn:    set_{max/active}_cpu_count()
 *
 *  @brief: Setting new cpu counts
 *
 *  @note:  Setting max cpu count is disabled. This value can be set
 *          only at the config file.
 *
 ******************************************************************/

void ShoreEnv::set_active_cpu_count(const unsigned actcpucnt)
{
    _active_cpu_count = ( actcpucnt>0 ? actcpucnt : _active_cpu_count );
}



/** Helper functions */


/******************************************************************
 *
 *  @fn:     _set_sys_params()
 *
 *  @brief:  Sets system params
 *
 *  @return: Returns 0 on success
 *
 ******************************************************************/

int ShoreEnv::_set_sys_params()
{
    // procmonitor returns 0 if it cannot find the number of processors
    if (_max_cpu_count==0) {
        _max_cpu_count = atoi(_sys_opts[SHORE_SYS_OPTIONS[0][0]].c_str());
    }

    // Set active CPU info
    uint tmp_active_cpu_count = atoi(_sys_opts[SHORE_SYS_OPTIONS[1][0]].c_str());
    if (tmp_active_cpu_count>_max_cpu_count) {
        _active_cpu_count = _max_cpu_count;
    }
    else {
        _active_cpu_count = tmp_active_cpu_count;
    }
    print_cpus();

    _activation_delay = atoi(_sys_opts[SHORE_SYS_OPTIONS[4][0]].c_str());
    return (0);
}


void ShoreEnv::print_cpus() const
{
    TRACE( TRACE_ALWAYS, "MaxCPU=(%d) - ActiveCPU=(%d)\n",
           _max_cpu_count, _active_cpu_count);
}


/******************************************************************
 *
 *  @fn:    readconfig
 *
 *  @brief: Reads configuration file
 *
 ******************************************************************/

void ShoreEnv::readconfig()
{
    string conf_file;
    envVar* ev = envVar::instance();
    conf_file = ev->getConfFile();

    TRACE( TRACE_ALWAYS, "Reading config file (%s)\n", conf_file.c_str());

    string tmp;
    int i=0;

    // Parse SYSTEM parameters
    TRACE( TRACE_DEBUG, "Reading SYS options\n");
    for (i=0; i<SHORE_NUM_SYS_OPTIONS; i++) {
        tmp = ev->getVar(SHORE_SYS_OPTIONS[i][0],SHORE_SYS_OPTIONS[i][1]);
        _sys_opts[SHORE_SYS_OPTIONS[i][0]] = tmp;
    }

    // Parse SYS-SM (database-independent) parameters
    TRACE( TRACE_DEBUG, "Reading SYS-SM options\n");
    for (i=0; i<SHORE_NUM_SYS_SM_OPTIONS; i++) {
        tmp = ev->getVar(SHORE_SYS_SM_OPTIONS[i][1],SHORE_SYS_SM_OPTIONS[i][2]);
        _sm_opts[SHORE_SYS_SM_OPTIONS[i][0]] = tmp;
    }

    //ev->printVars();
}



/******************************************************************
 *
 * @fn:    restart()
 *
 * @brief: Re-starts the environment
 *
 * @note:  - Stops everything
 *         - Rereads configuration
 *         - Starts everything
 *
 ******************************************************************/

int ShoreEnv::restart()
{
    TRACE( TRACE_DEBUG, "Restarting (%s)...\n", _sysname.c_str());
    stop();
    conf();
    _set_sys_params();
    start();
    return(0);
}


/******************************************************************
 *
 *  @fn:    conf
 *
 *  @brief: Prints configuration
 *
 ******************************************************************/

int ShoreEnv::conf()
{
    TRACE( TRACE_DEBUG, "ShoreEnv configuration\n");

    // Print storage manager options
    map<string,string>::iterator iter;
    TRACE( TRACE_DEBUG, "** SYS options\n");
    for ( iter = _sys_opts.begin(); iter != _sys_opts.end(); iter++)
        cout << "(" << iter->first << ") (" << iter->second << ")" << endl;

    TRACE( TRACE_DEBUG, "** SM options\n");
    for ( iter = _sm_opts.begin(); iter != _sm_opts.end(); iter++)
        cout << "(" << iter->first << ") (" << iter->second << ")" << endl;

    return (0);
}



/******************************************************************
 *
 *  @fn:    {enable,disable}_fake_disk_latency
 *
 *  @brief: Enables/disables the fake IO disk latency
 *
 ******************************************************************/

int ShoreEnv::disable_fake_disk_latency()
{
    // Disabling fake io delay, if any
    w_rc_t e = _pssm->disable_fake_disk_latency(_vid);
    if (e.is_error()) {
        TRACE( TRACE_ALWAYS, "Problem in disabling fake IO delay [0x%x]\n",
               e.err_num());
        return (1);
    }
    envVar* ev = envVar::instance();
    ev->setVarInt("shore-fakeiodelay-enable",0);
    ev->setVarInt("shore-fakeiodelay",0);
    return (0);
}

int ShoreEnv::enable_fake_disk_latency(const int adelay)
{
    if (!adelay>0) return (1);

    // Enabling fake io delay
    w_rc_t e = _pssm->set_fake_disk_latency(_vid,adelay);
    if (e.is_error()) {
        TRACE( TRACE_ALWAYS, "Problem in setting fake IO delay [0x%x]\n",
               e.err_num());
        return (2);
    }

    e = _pssm->enable_fake_disk_latency(_vid);
    if (e.is_error()) {
        TRACE( TRACE_ALWAYS, "Problem in enabling fake IO delay [0x%x]\n",
               e.err_num());
        return (3);
    }
    envVar* ev = envVar::instance();
    ev->setVarInt("shore-fakeiodelay-enable",1);
    ev->setVarInt("shore-fakeiodelay",adelay);
    return (0);
}



/******************************************************************
 *
 *  @fn:    dump()
 *
 *  @brief: Dumps the data
 *
 ******************************************************************/

int ShoreEnv::dump()
{
    TRACE( TRACE_DEBUG, "~~~~~~~~~~~~~~~~~~~~~\n");
    TRACE( TRACE_DEBUG, "Dumping Shore Data\n");

    TRACE( TRACE_ALWAYS, "Not implemented...\n");

    TRACE( TRACE_DEBUG, "~~~~~~~~~~~~~~~~~~~~~\n");
    return (0);
}



/******************************************************************
 *
 *  @fn:    setAsynchCommit()
 *
 *  @brief: Sets whether asynchronous commit will be used or not
 *
 ******************************************************************/

void ShoreEnv::setAsynchCommit(const bool bAsynch)
{
    _asynch_commit = bAsynch;
}


#if 0

/******************************************************************
 *
 *  @fn:    start_flusher()
 *
 *  @brief: Starts the baseline flusher
 *
 ******************************************************************/

int ShoreEnv::_start_flusher()
{
    _base_flusher = new flusher_t(this,std::string("base-flusher"));
    assert (_base_flusher);
    _base_flusher->fork();
    _base_flusher->start();
    return (0);
}


/******************************************************************
 *
 *  @fn:    stop_flusher()
 *
 *  @brief: Stops the baseline flusher
 *
 ******************************************************************/

int ShoreEnv::_stop_flusher()
{
    _base_flusher->stop();
    _base_flusher->join();
    return (0);
}


/******************************************************************
 *
 *  @fn:    to_base_flusher()
 *
 *  @brief: Enqueues a request to the base flusher
 *
 ******************************************************************/

void ShoreEnv::to_base_flusher(Request* ar)
{
// TODO: IP: Add multiple flusher to the baseline as well
    _base_flusher->enqueue_toflush(ar);
}


/******************************************************************
 *
 *  @fn:    db_print_init
 *
 *  @brief: Starts the db printer thread and then deletes it
 *
 ******************************************************************/

void ShoreEnv::db_print_init(int num_lines)
{
    table_printer_t* db_printer = new table_printer_t(this, num_lines);
    db_printer->fork();
    db_printer->join();
    delete (db_printer);
}


/******************************************************************
 *
 *  @fn:    db_fetch_init
 *
 *  @brief: Starts the db fetcher thread and then deletes it
 *
 ******************************************************************/

void ShoreEnv::db_fetch_init()
{
    table_fetcher_t* db_fetcher = new table_fetcher_t(this);
    db_fetcher->fork();
    db_fetcher->join();
    delete (db_fetcher);
}
#endif
