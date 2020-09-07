/* Copyright (C) 2006-2008 Jeff Epler <jepler@unpythonic.net>
 * Copyright (C) 2012-2014 Michael Haberler <license@mah.priv.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * TODO: on setuid and capabilites(7)
 *
 * right now this program runs as setuid root
 * it might be possible to drop the wholesale root privs by using
 * capabilites(7). in particular:
 *
 * CAP_SYS_RAWIO   open /dev/mem and /dev/kmem & Perform I/O port operations
 * CAP_SYS_NICE    set real-time scheduling policies, set CPU affinity
 *
 * NB:  Capabilities are a per-thread attribute,
 * so this might need to be done on a per-thread basis
 * see also CAP_SETPCAP, CAP_INHERITABLE and 'inheritable set'
 *
 * see also:
 * http://stackoverflow.com/questions/13183327/drop-root-uid-while-retaining-cap-sys-nice
 * http://stackoverflow.com/questions/12141420/losing-capabilities-after-setuid
 */

#include "config.h"
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <sys/resource.h>
#include <linux/capability.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <errno.h>
#include <malloc.h>
#include <assert.h>
#include <limits.h>
#include <sys/prctl.h>
#include <mk-inifile.h>

#include <czmq.h>
#include <google/protobuf/text_format.h>

#include <message.pb.h>
#include <pbutil.hh>  // note_printf(machinetalk::Container &c, const char *fmt, ...)

using namespace google::protobuf;

#include "rtapi.h"
#include "rtapi_global.h"
#include "rtapi_export.h"
#include "rtapi_flavor.h"  // flavor_*
#include "hal.h"
#include "hal_priv.h"
#include "shmdrv.h"

#ifdef HAVE_SYS_IO_H
#include "rtapi_io.h"
#endif

#include "mk-backtrace.h"
#include "setup_signals.h"
#include "mk-zeroconf.hh"

#include "rtapi_app_module.hh"

#define BACKGROUND_TIMER 1000
#define HALMOD   "hal_lib"
#define RTAPIMOD "rtapi"

using namespace std;

/* Pre-allocation size. Must be enough for the whole application life to avoid
 * pagefaults by new memory requested from the system. */
#define PRE_ALLOC_SIZE		(30 * 1024 * 1024)

typedef int (*hal_call_usrfunct_t)(const char *name,
				   const int argc,
				   const char **argv,
				   int *ureturn);
static hal_call_usrfunct_t call_usrfunct;

typedef int (*halg_foreach_t)(bool use_hal_mutex,
			      foreach_args_t *args,
			      hal_object_callback_t callback);
typedef int (*hal_exit_usercomps_t)(char *);

static std::map<string, Module> modules;
static std::map<string, pbstringarray_t> iparms; // default instance params

static std::vector<string> loading_order;
static void remove_module(std::string name);

static struct rusage rusage;
static unsigned long minflt, majflt;
static int rtapi_instance_loc;
static int use_drivers = 0;
static int foreground;
static int debug = 0;
static int signal_fd;
static bool interrupted;
static bool trap_signals = true;
int shmdrv_loaded;
long page_size;
static const char *progname;
static const char *z_uri;
static int z_port;
static int z_debug = 0;
static uuid_t process_uuid;
static char process_uuid_str[40];
static register_context_t *rtapi_publisher;
static const char *service_uuid;

#ifdef NOTYET
static int remote = 0; // announce and bind a TCP socket
static const char *ipaddr = "127.0.0.1";
static const char *z_uri_dsn;
#endif

static const char *interfaces;
static const char *inifile;
static FILE *inifp;

#ifdef NOTYET
static AvahiCzmqPoll *av_loop;
#endif

// RTAPI flavor functions are dynamically linked in through rtapi.so
// - Pointers to functions
flavor_name_t * flavor_name_ptr;
flavor_names_t * flavor_names_ptr;
flavor_is_configured_t * flavor_is_configured_ptr;
flavor_feature_t * flavor_feature_ptr;
flavor_byname_t * flavor_byname_ptr;
flavor_default_t * flavor_default_ptr;
flavor_install_t * flavor_install_ptr;
// - Keep track of whether pointers have been set
static int have_flavor_funcs = 0;
// - For storing the `-f` option until flavors are ready to be configured
static char flavor_name_opt[MAX_FLAVOR_NAME_LEN] = {0};

// global_data is set in attach_global_segment() which was already
// created by rtapi_msgd
global_data_t *global_data;
static int init_actions(int instance);
static void exit_actions(int instance);
static int harden_rt(void);
static int record_instparms(Module &mi);

static void configure_flavor(machinetalk::Container &pbreply)
{

    // Retrieve the flavor_*_ptr() addresses so rtapi flavor functions can be
    // called.
    if (have_flavor_funcs) return;  // Already did this

    // Load pointers
#    define GET_FLAVOR_FUNC(name) do {                          \
        name ## _ptr = mi.sym<name ## _t *>(#name);             \
        if (name ## _ptr == NULL && mi.errmsg)                  \
            note_printf(pbreply, "BUG: %s:", mi.errmsg);        \
        assert(name ## _ptr != NULL);                           \
    } while (0)

    Module &mi = modules[RTAPIMOD];

    GET_FLAVOR_FUNC(flavor_name);
    GET_FLAVOR_FUNC(flavor_names);
    GET_FLAVOR_FUNC(flavor_is_configured);
    GET_FLAVOR_FUNC(flavor_feature);
    GET_FLAVOR_FUNC(flavor_byname);
    GET_FLAVOR_FUNC(flavor_default);
    GET_FLAVOR_FUNC(flavor_install);

    flavor_descriptor_ptr flavor = NULL;
    if (flavor_name_opt[0]) {
        // Configure flavor from -f cmdline arg
        if ((flavor = (*flavor_byname_ptr)(flavor_name_opt)) == NULL) {
            fprintf(stderr, "No such flavor '%s'; valid flavors are:\n",
                    flavor_name_opt);
            flavor_descriptor_ptr * f_handle;
            const char * fname;
            for (f_handle=NULL; (fname=(*flavor_names_ptr)(&f_handle)); )
                fprintf(stderr, "      %s\n", fname);
            exit(1);
        }
    } else {
        // Configure flavor from environment or automatically
        flavor = (*flavor_default_ptr)();  // Exits on error
    }
    (*flavor_install_ptr)(flavor);  // Exits on error
    have_flavor_funcs = 1;  // Don't run this again
}


static int do_one_item(char item_type_char,
		       const string &param_name,
		       const string &param_value,
		       void *vitem,
		       int idx,
		       machinetalk::Container &pbreply)
{
    char *endp;
    switch(item_type_char) {
    case 'l': {
	long *litem = *(long**) vitem;
	litem[idx] = strtol(param_value.c_str(), &endp, 0);
	if(*endp) {
	    note_printf(pbreply, "`%s' invalid for parameter `%s'",
			param_value.c_str(), param_name.c_str());
	    return -1;
	}
	return 0;
    }
    case 'i': {
	int *iitem = *(int**) vitem;
	iitem[idx] = strtol(param_value.c_str(), &endp, 0);
	if(*endp) {
	    note_printf(pbreply,
			"`%s' invalid for parameter `%s'",
			param_value.c_str(), param_name.c_str());
	    return -1;
	}
	return 0;
    }
    case 'u': {
	unsigned *iitem = *(unsigned**) vitem;
	iitem[idx] = strtoul(param_value.c_str(), &endp, 0);
	if(*endp) {
	    note_printf(pbreply,
			"`%s' invalid for parameter `%s'",
			param_value.c_str(), param_name.c_str());
	    return -1;
	}
	return 0;
    }
    case 's': {
	char **sitem = *(char***) vitem;
	sitem[idx] = strdup(param_value.c_str());
	return 0;
    }
    default:
	note_printf(pbreply,
		    "%s: Invalid type character `%c'\n",
		    param_name.c_str(), item_type_char);
	return -1;
    }
    return 0;
}

void remove_quotes(string &s)
{
    s.erase(remove_copy(s.begin(), s.end(), s.begin(), '"'), s.end());
}

static int do_module_args(Module &mi,
			  pbstringarray_t args,
			  const string &symprefix,
			  machinetalk::Container &pbreply)
{
    for(int i = 0; i < args.size(); i++) {
        string s(args.Get(i));
	remove_quotes(s);
        size_t idx = s.find('=');
        if(idx == string::npos) {
	    note_printf(pbreply, "Invalid parameter `%s'",
			s.c_str());
            return -1;
        }
        string param_name(s, 0, idx);
        string param_value(s, idx+1);
        void *item = mi.sym<void*>(symprefix + "address_" + param_name);
        if (!item) {
	    note_printf(pbreply,
			"Unknown parameter `%s'",
			s.c_str());
            return -1;
        }
        char **item_type = mi.sym<char**>(symprefix + "type_" + param_name);
        if (!item_type || !*item_type) {
	    if (mi.errmsg)
		note_printf(pbreply, "BUG: %s:", mi.errmsg);
	    note_printf(pbreply,
			"Unknown parameter `%s' (type information missing)",
			s.c_str());
            return -1;
        }
        string item_type_string = *item_type;

        if (item_type_string.size() > 1) {
            int a, b;
            char item_type_char;
            int r = sscanf(item_type_string.c_str(), "%d-%d%c",
			   &a, &b, &item_type_char);
            if(r != 3) {
		note_printf(pbreply,
			    "Unknown parameter `%s'"
			    " (corrupt array type information): %s",
			    s.c_str(), item_type_string.c_str());
                return -1;
            }
            size_t idx = 0;
            int i = 0;
            while(idx != string::npos) {
                if(i == b) {
		    note_printf(pbreply,
				"%s: can only take %d arguments",
				s.c_str(), b);
                    return -1;
                }
                size_t idx1 = param_value.find(",", idx);
                string substr(param_value, idx, idx1 - idx);
                int result = do_one_item(item_type_char, s, substr, item, i, pbreply);
                if(result != 0) return result;
                i++;
                idx = idx1 == string::npos ? idx1 : idx1 + 1;
            }
        } else {
            char item_type_char = item_type_string[0];
            int result = do_one_item(item_type_char, s, param_value, item, 0, pbreply);
            if (result != 0) return result;
        }
    }
    return 0;
}

static const char **pbargv(const pbstringarray_t &args)
{
    const char **argv, **s;
    s = argv = (const char **) calloc(sizeof(char *), args.size() + 1);
    for (int i = 0; i < args.size(); i++) {
	*s++ = args.Get(i).c_str();
    }
    *s = NULL;
    return argv;
}

static void usrfunct_error(const int retval,
			   const string &func,
			   pbstringarray_t args,
			   machinetalk::Container &pbreply)
{
    if (retval >= 0) return;
    string s = pbconcat(args);
    note_printf(pbreply, "hal_call_usrfunct(%s,%s) failed: %d - %s",
		func.c_str(), s.c_str(), retval, strerror(-retval));
}

// separate instance args of the legacy type (name=value)
// from any other non-legacy params which should go into newinst argv
//
// given an args array like:
// foo=bar baz=123 -- blah=fasel --foo=123 -c
//
// '--' is skipped and is the separator between legacy args
// and any others passed to newinst as argc/argv
//
// the above arguments are split into
// kvpairs:   foo=bar baz=123
// leftovers:     blah=fasel --foo=123 -c
//
// scenario 2 - kv pairs followed by getop-style options:
// foo=bar baz=123 --baz --fasel=123 -c blah=4711
//
// the above arguments are split into
// kvpairs:   foo=bar baz=123
// leftovers:  --baz --fasel=123 -c blah=4711
//
// i.e. any argument following an option starting with '--' is
// treated as leftovers argument, even if it has a key=value syntax
//

static void separate_kv(pbstringarray_t &kvpairs,
		    pbstringarray_t &leftovers,
		    const pbstringarray_t &args)
{
bool extra = false;
string prefix = "--";

    for(int i = 0; i < args.size(); i++) {
        string s(args.Get(i));
	remove_quotes(s);
	if (s == prefix) { // standalone separator '--'
	    extra = true;
	    continue; // skip this argument
	}
	// no separator, but an option starting with -- like '--foo'
	// pass this, and any following arguments and options to leftovers
	if (std::equal(prefix.begin(), prefix.end(), s.begin()))
	    extra = true;

        if (extra)
	    leftovers.Add()->assign(s);
	else
	    kvpairs.Add()->assign(s);
    }
}

static int do_newinst_cmd(int instance,
			  string comp,
			  string instname,
			  pbstringarray_t args,
			  machinetalk::Container &pbreply)
{
    int retval = -1;


    if (call_usrfunct == NULL) {
        pbreply.set_retcode(1);
        pbreply.add_note("this HAL library version does "
                         "not support user functions - version problem?");
        return -1;
    }
    if (modules.count(comp) == 0) {
        // if newinst via halcmd, it should have been automatically loaded already
        note_printf(pbreply,
                    "newinst: component '%s' not loaded",
                    comp.c_str());
        return -1;
    }
    Module &mi = modules[comp];

    string s = pbconcat(iparms[mi.name]);

    // set the default instance parameters which were recorded during
    // initial load with record_instanceparams()
    retval = do_module_args(mi, iparms[mi.name], RTAPI_IP_SYMPREFIX, pbreply);
    if (retval < 0) {
        note_printf(pbreply,
                    "passing default instance args for '%s' failed: '%s'",
                    instname.c_str(), s.c_str());
        return retval;
    }

    s = pbconcat(args);
    pbstringarray_t kvpairs, leftovers;
    separate_kv(kvpairs, leftovers, args);

    // set the instance parameters
    retval = do_module_args(mi, kvpairs, RTAPI_IP_SYMPREFIX, pbreply);
    if (retval < 0) {
        note_printf(pbreply,
                    "passing args for '%s' failed: '%s'",
                    instname.c_str(), s.c_str());
        return retval;
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
                    "%s: instargs='%s'\n",__FUNCTION__,
                    s.c_str());

    // massage the argv for the newinst user function,
    // and call it
    pbstringarray_t a;
    a.Add()->assign(comp);
    a.Add()->assign(instname);
    a.MergeFrom(leftovers);
    const char **argv = pbargv(a); // pass non-kv pairs only
    int ureturn = 0;
    retval = call_usrfunct("newinst", a.size(), argv, &ureturn );
    if (argv) free(argv);
    if (retval == 0) retval = ureturn;
    usrfunct_error(retval, "newinst", args, pbreply);
    return retval;
}

static int do_delinst_cmd(int instance,
			  string instname,
			  machinetalk::Container &pbreply)
{
    int retval = -1;
    string s;


    if (call_usrfunct == NULL) {
        pbreply.set_retcode(1);
        pbreply.add_note("this HAL library version does not support "
                         "user functions - version problem?");
        return -1;
    }
    pbstringarray_t a;
    a.Add()->assign(instname);
    const char **argv = pbargv(a);
    int ureturn = 0;
    retval = call_usrfunct("delinst", a.size(), argv, &ureturn);
    if (argv) free(argv);
    if (retval == 0) retval = ureturn;
    usrfunct_error(retval, "delinst", a, pbreply);
    return retval;
 }

static int do_callfunc_cmd(int instance,
			   string func,
			   pbstringarray_t args,
			   machinetalk::Container &pbreply)
{
    int retval = -1;

    if (call_usrfunct == NULL) {
        pbreply.set_retcode(1);
        pbreply.add_note("this HAL library version does not support user "
                         "functions - version problem?");
        return -1;
    }
    const char **argv = pbargv(args);
    int ureturn = 0;
    retval = call_usrfunct(func.c_str(),
                           args.size(),
                           argv,
                           &ureturn);
    if (argv) free(argv);
    if (retval == 0) retval = ureturn;
    usrfunct_error(retval, func, args, pbreply);
    return retval;
}



static int do_load_cmd(int instance,
		       string path,
		       pbstringarray_t args,
		       machinetalk::Container &pbreply)
{
    Module mi;

    if (mi.load(path) != 0) {
        note_printf(pbreply, "%s: %s", __FUNCTION__, mi.errmsg);
        return -1;
    }

    if (modules.count(mi.name) != 0) {
        note_printf(pbreply, "%s: already loaded\n", mi.name.c_str());
	return -1;
    }

    // Success
    rtapi_print_msg(RTAPI_MSG_DBG, "Loaded module %s from path %s\n",
                    mi.name.c_str(), mi.path().c_str());
    // first load of a module. Record default instanceparams
    // so they can be replayed before newinst
    record_instparms(mi);

    int (*rtapi_app_main_dlsym)(void) =
        mi.sym<int(*)(void)>("rtapi_app_main");
    if (!rtapi_app_main_dlsym) {
        note_printf(pbreply, "%s: dlsym: %s\n",
                    mi.name.c_str(), mi.errmsg);
        return -1;
    }
    int result;

    result = do_module_args(mi, args, RTAPI_MP_SYMPREFIX, pbreply);
    if (result < 0) {
        mi.close();
        return -1;
    }

    // Configure flavor, needed even before `rtapi_app_main_dlsym()` runs
    // next.  This runs at every module load, but only does anything after
    // RTAPIMOD, the first module loaded.
    configure_flavor(pbreply);

    // need to call rtapi_app_main with as root
    // RT thread creation and hardening requires this
    if ((result = rtapi_app_main_dlsym()) < 0) {
        note_printf(pbreply, "rtapi_app_main(%s): %d %s\n",
                    mi.name.c_str(), result, strerror(-result));
        return result;
    }
    modules[mi.name] = mi;
    loading_order.push_back(mi.name);

    rtapi_print_msg(RTAPI_MSG_DBG, "%s: loaded from %s\n",
                    mi.name.c_str(), mi.path().c_str());
    return 0;
}

 static int do_unload_cmd(int instance, string name, machinetalk::Container &reply)
{

    int retval = 0;

    if (modules.count(name) == 0) {
	note_printf(reply, "unload: '%s' not loaded\n",
		    name.c_str());
	return -1;
    } else {
	Module &mi = modules[name];
        int (*stop)(void) = mi.sym<int(*)(void)>("rtapi_app_exit");
        if (stop)
            stop();
        mi.close();
        modules.erase(modules.find(name));
        remove_module(name);
    }
    rtapi_print_msg(RTAPI_MSG_DBG, " '%s' unloaded\n", name.c_str());
    return retval;
}

static int exit_usercomps(char *name)
{
    Module &hallib = modules[HALMOD];

    hal_exit_usercomps_t huc =
        hallib.sym<hal_exit_usercomps_t>("hal_exit_usercomps");
    return huc(NULL);
}

static int stop_threads(void)
{
    typedef int (*f_t)(void);
    Module &hallib = modules[HALMOD];
    f_t hst = hallib.sym<f_t>("hal_stop_threads");
    return hst();
}

// shut down the stack in reverse loading order
static void exit_actions(int instance)
{
    machinetalk::Container reply;

    stop_threads();
    sleep(0.2);
    exit_usercomps(NULL);

    size_t index = loading_order.size() - 1;
    for(std::vector<std::string>::reverse_iterator rit = loading_order.rbegin();
	rit != loading_order.rend(); ++rit, --index) {
	do_unload_cmd(instance, *rit, reply);
    }
}

static int init_actions(int instance)
{
    int retval;

    machinetalk::Container reply;

    retval =  do_load_cmd(instance, RTAPIMOD, pbstringarray_t(), reply);
    if (retval)
	return retval;
    if ((retval = do_load_cmd(instance, HALMOD, pbstringarray_t(), reply)))
	return retval;

    // resolve the "hal_call_usrfunct" for later
    // callfunc, newinst & delinst need it
    Module &hallib = modules[HALMOD];
    call_usrfunct = hallib.sym<hal_call_usrfunct_t>("hal_call_usrfunct");

    if (call_usrfunct == NULL) {
        rtapi_print_msg(RTAPI_MSG_ERR,
                        "cant resolve 'hal_call_usrfunct' in "
                        "hal_lib - version problem?");
        if (hallib.errmsg)
            rtapi_print_msg(
                RTAPI_MSG_ERR, "dlsym(hal_call_usrfunct): '%s'", hallib.errmsg);
        return -1;
    }
    return 0;
}


static int attach_global_segment()
{
    int retval = 0;
    int globalkey = OS_KEY(GLOBAL_KEY, rtapi_instance_loc);
    int size = 0;
    int tries = 10; // 5 sec deadline for msgd/globaldata to come up

    shm_common_init();
    do {
	retval = shm_common_new(globalkey, &size,
				rtapi_instance_loc, (void **) &global_data, 0);
	if (retval < 0) {
	    tries--;
	    if (tries == 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		       "rtapi_app:%d: ERROR: cannot attach global segment key=0x%x %s\n",
		       rtapi_instance_loc, globalkey, strerror(-retval));
		return retval;
	    }
	    struct timespec ts = {0, 500 * 1000 * 1000}; //ms
	    nanosleep(&ts, NULL);
	}
    } while (retval < 0);

    if (size < (int) sizeof(global_data_t)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	       "rtapi_app:%d global segment size mismatch: expect >%zu got %d\n",
	       rtapi_instance_loc, sizeof(global_data_t), size);
	return -EINVAL;
    }

    tries = 10;
    while  (global_data->magic !=  GLOBAL_READY) {
	tries--;
	if (tries == 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		   "rtapi_app:%d: ERROR: global segment magic not changing to ready: magic=0x%x\n",
		   rtapi_instance_loc, global_data->magic);
	    return -EINVAL;
	}
	struct timespec ts = {0, 500 * 1000 * 1000}; //ms
	nanosleep(&ts, NULL);
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
                 "rtapi_app:%d: Attached global segment magic=0x%x\n",
                 rtapi_instance_loc, global_data->magic);

    return retval;
}


// handle commands from zmq socket
static int rtapi_request(zloop_t *loop, zsock_t *socket, void *arg)
{
    zmsg_t *r = zmsg_recv(socket);
    char *origin = zmsg_popstr (r);
    zframe_t *request_frame  = zmsg_pop (r);
    static bool force_exit = false;
    Module mi;
    int retval;
    int (*create_thread)(const hal_threadargs_t*);
    int (*delete_thread)(const char *);

    if(request_frame == NULL){
	rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_request(): NULL zframe_t 'request_frame' passed");
	return -1;
	}

    machinetalk::Container pbreq, pbreply;

    if (!pbreq.ParseFromArray(zframe_data(request_frame),
			      zframe_size(request_frame))) {
	rtapi_print_msg(RTAPI_MSG_ERR, "cant decode request from %s (size %zu)",
			origin ? origin : "NULL",
			zframe_size(request_frame));
	zmsg_destroy(&r);
	return 0;
    }
    if (z_debug) {
	string buffer;
	if (TextFormat::PrintToString(pbreq, &buffer)) {
	    fprintf(stderr, "request: %s\n",buffer.c_str());
	}
    }

    pbreply.set_type(machinetalk::MT_RTAPI_APP_REPLY);

    switch (pbreq.type()) {
    case machinetalk::MT_RTAPI_APP_PING:
	char buffer[RTAPI_LINELEN];
	snprintf(buffer, sizeof(buffer),
		 "pid=%d flavor=%s gcc=%s git=%s",
		 getpid(), (*flavor_name_ptr)(NULL),  __VERSION__, GIT_VERSION);
	pbreply.add_note(buffer);
	pbreply.set_retcode(0);
	break;

    case machinetalk::MT_RTAPI_APP_EXIT:
	assert(pbreq.has_rtapicmd());
	exit_actions(pbreq.rtapicmd().instance());
	force_exit = true;
	pbreply.set_retcode(0);
	break;

    case machinetalk::MT_RTAPI_APP_CALLFUNC:

	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_func());
	assert(pbreq.rtapicmd().has_instance());
	pbreply.set_retcode(do_callfunc_cmd(pbreq.rtapicmd().instance(),
					      pbreq.rtapicmd().func(),
					      pbreq.rtapicmd().argv(),
					      pbreply));
	break;

    case machinetalk::MT_RTAPI_APP_NEWINST:
	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_comp());
	assert(pbreq.rtapicmd().has_instname());
	assert(pbreq.rtapicmd().has_instance());
	pbreply.set_retcode(do_newinst_cmd(pbreq.rtapicmd().instance(),
					   pbreq.rtapicmd().comp(),
					   pbreq.rtapicmd().instname(),
					   pbreq.rtapicmd().argv(),
					   pbreply));
	break;

    case machinetalk::MT_RTAPI_APP_DELINST:

	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_instname());
	assert(pbreq.rtapicmd().has_instance());
	pbreply.set_retcode(do_delinst_cmd(pbreq.rtapicmd().instance(),
					   pbreq.rtapicmd().instname(),
					   pbreply));
	break;


    case machinetalk::MT_RTAPI_APP_LOADRT:
	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_modname());
	assert(pbreq.rtapicmd().has_instance());
	pbreply.set_retcode(do_load_cmd(pbreq.rtapicmd().instance(),
					pbreq.rtapicmd().modname(),
					pbreq.rtapicmd().argv(),
					pbreply));
	break;

    case machinetalk::MT_RTAPI_APP_UNLOADRT:
	assert(pbreq.rtapicmd().has_modname());
	assert(pbreq.rtapicmd().has_instance());

	pbreply.set_retcode(do_unload_cmd(pbreq.rtapicmd().instance(),
					  pbreq.rtapicmd().modname(),
					  pbreply));
	break;

    case machinetalk::MT_RTAPI_APP_LOG:
	assert(pbreq.has_rtapicmd());
	if (pbreq.rtapicmd().has_rt_msglevel()) {
	    global_data->rt_msg_level = pbreq.rtapicmd().rt_msglevel();
	}
	if (pbreq.rtapicmd().has_user_msglevel()) {
	    global_data->user_msg_level = pbreq.rtapicmd().user_msglevel();
	}
	pbreply.set_retcode(0);
	break;

    case machinetalk::MT_RTAPI_APP_NEWTHREAD:
	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_threadname());
	assert(pbreq.rtapicmd().has_threadperiod());
	assert(pbreq.rtapicmd().has_cpu());
	assert(pbreq.rtapicmd().has_use_fp());
	assert(pbreq.rtapicmd().has_instance());
	assert(pbreq.rtapicmd().has_flags());

        if (modules.count(HALMOD)  == 0) {
            pbreply.add_note("hal_lib not loaded");
            pbreply.set_retcode(-1);
            break;
        }
        mi = modules[HALMOD];
        create_thread =
            mi.sym<int(*)(const hal_threadargs_t*)>("hal_create_xthread");
        if (create_thread == NULL) {
            pbreply.add_note("symbol 'hal_create_thread' not found in hal_lib");
            pbreply.set_retcode(-1);
            break;
        }
        hal_threadargs_t args;
        args.name = pbreq.rtapicmd().threadname().c_str();
        args.period_nsec = pbreq.rtapicmd().threadperiod();
        args.uses_fp = pbreq.rtapicmd().use_fp();
        args.cpu_id = pbreq.rtapicmd().cpu();
        args.flags = (rtapi_thread_flags_t) pbreq.rtapicmd().flags();
        strncpy(args.cgname, pbreq.rtapicmd().cgname().c_str(), RTAPI_LINELEN-1);

        retval = create_thread(&args);
        if (retval < 0) {
            pbreply.add_note("hal_create_xthread() failed, see log");
        }
        pbreply.set_retcode(retval);
	break;

    case machinetalk::MT_RTAPI_APP_DELTHREAD:
	assert(pbreq.has_rtapicmd());
	assert(pbreq.rtapicmd().has_threadname());
	assert(pbreq.rtapicmd().has_instance());

        if (modules.count(HALMOD) == 0) {
            pbreply.add_note("hal_lib not loaded");
            pbreply.set_retcode(-1);
            break;
        }
        mi = modules[HALMOD];
        delete_thread = mi.sym<int(*)(const char *)>("hal_thread_delete");
        if (delete_thread == NULL) {
            pbreply.add_note("symbol 'hal_thread_delete' not found in hal_lib");
            pbreply.set_retcode(-1);
            break;
        }
        retval = delete_thread(pbreq.rtapicmd().threadname().c_str());
        pbreply.set_retcode(retval);
	break;

    default:
	rtapi_print_msg(RTAPI_MSG_ERR,
			"unkown command type %d)",
			(int) pbreq.type());
	zmsg_destroy(&r);
	return 0;


    }
    // log accumulated notes
    for (int i = 0; i < pbreply.note_size(); i++) {
	rtapi_print_msg(pbreply.retcode() ? RTAPI_MSG_ERR : RTAPI_MSG_DBG,
			"%s", pbreply.note(i).c_str());
    }

    // TODO: extract + attach error message

    size_t reply_size = pbreply.ByteSize();
    zframe_t *reply = zframe_new (NULL, reply_size);
    if(reply == NULL){
	rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_request(): NULL zframe_t 'reply' passed");
	return -1;
	}

    if (!pbreply.SerializeWithCachedSizesToArray(zframe_data (reply))) {
	zframe_destroy(&reply);
	rtapi_print_msg(RTAPI_MSG_ERR,
			"cant serialize to %s (type %d size %zu)",
			origin ? origin : "NULL",
			pbreply.type(),
			reply_size);
    } else {
	if (z_debug) {
	    string buffer;
	    if (TextFormat::PrintToString(pbreply, &buffer)) {
		fprintf(stderr, "reply: %s\n",buffer.c_str());
	    }
	}
	assert(zstr_sendm (socket, origin) == 0);
	if (zframe_send (&reply, socket, 0)) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "cant serialize to %s (type %d size %zu)",
			    origin ? origin : "NULL",
			    pbreply.type(),
			    zframe_size(reply));
	}
    }
    free(origin);
    zmsg_destroy(&r);
    if (force_exit) // terminate the zloop
	return -1;
    return 0;
}

static void btprint(const char *prefix, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    rtapi_msg_handler_t  print = rtapi_get_msg_handler();
    print(RTAPI_MSG_ERR, fmt, args);
    va_end(args);
}

// handle signals delivered via sigaction - not all signals
// can be dealt with through signalfd(2)
// log, try to do something sane, and dump core
static void sigaction_handler(int sig, siginfo_t *si, void *uctx)
{

    switch (sig) {
    case SIGXCPU:
        // should not happen - must be handled in RTAPI if enabled
        rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_app: BUG: SIGXCPU should be handled in RTAPI");
	// NB: fall through

    default:
	rtapi_print_msg(RTAPI_MSG_ERR,
			"signal %d - '%s' received, dumping core (current dir=%s)",
			sig, strsignal(sig), get_current_dir_name());
	backtrace("", "rtapi_app", btprint, 3);
	if (global_data)
	    global_data->rtapi_app_pid = 0;

	// reset handler for current signal to default
        signal(sig, SIG_DFL);
	// and re-raise so we get a proper core dump and stacktrace
	kill(getpid(), sig);
	sleep(1);
        break;
    }
    // not reached
}


// handle signals delivered synchronously in the event loop
// by polling signal_fd
// log, start shutdown and flag end of the event loop
static int s_handle_signal(zloop_t *loop, zmq_pollitem_t *poller, void *arg)
{
    struct signalfd_siginfo fdsi;
    ssize_t s;

    s = read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"BUG: read(signal_fd): %s", strerror(errno));
	return 0;
    }
    switch (fdsi.ssi_signo) {
	// see machinetalk/lib/setup_signals for handled signals
    case SIGINT:
    case SIGQUIT:
    case SIGKILL:
    case SIGTERM:
	rtapi_print_msg(RTAPI_MSG_INFO,
			"signal %d - '%s' received, exiting",
			fdsi.ssi_signo, strsignal(fdsi.ssi_signo));
	exit_actions(rtapi_instance_loc);
	interrupted = true; // make mainloop exit
	if (global_data)
	    global_data->rtapi_app_pid = 0;
	return -1;

    default:
	// this should be handled either above or in sigaction_handler
	rtapi_print_msg(RTAPI_MSG_ERR, "BUG: unhandled signal %d - '%s' received\n",
			fdsi.ssi_signo, strsignal(fdsi.ssi_signo));
    }
    return 0;
}

static int
s_handle_timer(zloop_t *loop, int  timer_id, void *args)
{
    (void)loop;
    (void)timer_id;
    (void)args;
    if (global_data->rtapi_msgd_pid == 0) {
	// cant log this via rtapi_print, since msgd is gone
	rtapi_print_msg(RTAPI_MSG_ERR,"rtapi_msgd went away, exiting\n");
	exit_actions(rtapi_instance_loc);
	if (global_data)
	    global_data->rtapi_app_pid = 0;
	exit(EXIT_FAILURE);
    }
    return 0;
}


static int mainloop(size_t  argc, char **argv)
{
    int retval;
    unsigned i;

    // set new process name and clean out cl args
    memset(argv[0], '\0', strlen(argv[0]));
    snprintf(argv[0], 10, "rtapi:%d", rtapi_instance_loc);
    backtrace_init(argv[0]);

    for (i = 1; i < argc; i++)
	memset(argv[i], '\0', strlen(argv[i]));

    // Set name printed in logs
    rtapi_set_logtag("rtapi_app");

    // set this thread's name so it can be identified in ps/top as
    // rtapi:<instance>
    if (prctl(PR_SET_NAME, argv[0]) < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,	"rtapi_app: prctl(PR_SETNAME,%s) failed: %s\n",
	       argv[0], strerror(errno));
    }

    // attach global segment which rtapi_msgd already prepared
    if ((retval = attach_global_segment()) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "%s: FATAL - failed to attach to global segment\n",
	       argv[0]);
	exit(retval);
    }

    // make sure rtapi_msgd's pid is valid and msgd is running,
    // in case we caught a leftover shmseg
    // otherwise log messages would vanish
    if ((global_data->rtapi_msgd_pid == 0) ||
	kill(global_data->rtapi_msgd_pid, 0) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,"%s: rtapi_msgd pid invalid: %d, exiting\n",
	       argv[0], global_data->rtapi_msgd_pid);
	exit(EXIT_FAILURE);
    }

    // from here on it is safe to use the ring buffer message handler
    // since the error ring is now set up and msgd is logging it
    // if (! rtapi_message_buffer_ready()) {
    //   rtapi_print_msg(RTAPI_MSG_ERR, "Message buffer not initialized!  Exiting");
    //   exit(1);
    // }
    if (! foreground) {
        rtapi_set_msg_handler(ring_rtapi_msg_handler);
        rtapi_set_msg_level(global_data->rt_msg_level);
        rtapi_print_msg(RTAPI_MSG_INFO,"%s: rtapi_app logging through message ring\n",
                        argv[0]);
    } else {
        rtapi_print_msg(RTAPI_MSG_INFO,"%s: rtapi_app logging to console\n",
                        argv[0]);
    }

    // load rtapi and hal_lib
    // - After this, it's safe to run any flavor_* stuff
    if (init_actions(rtapi_instance_loc)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"init_actions() failed\n");
	global_data->rtapi_app_pid = 0;
	exit(1);
    }

    // make sure we're setuid root when we need to
    if (use_drivers || (*flavor_feature_ptr)(NULL, FLAVOR_DOES_IO)) {
	if (geteuid() != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "rtapi_app:%d need to"
			    " 'sudo make setuid' to access I/O?\n",
			    rtapi_instance_loc);
	    global_data->rtapi_app_pid = 0;
	    exit(EXIT_FAILURE);
	}
    }

    // assorted RT incantations - memory locking, prefaulting etc
    if ((retval = harden_rt())) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"rtapi_app:%d failed to setup "
			"realtime environment - 'sudo make setuid' missing?\n",
			rtapi_instance_loc);
	global_data->rtapi_app_pid = 0;
	exit(retval);
    }

    // block all signal delivery through signal handler
    // since we're using signalfd()
    // do this here so child threads inherit the sigmask
    if (trap_signals) {
	signal_fd = setup_signals(sigaction_handler, SIGINT, SIGQUIT, SIGKILL, SIGTERM, -1);
	assert(signal_fd > -1);
    }

    // suppress default handling of signals in zsock_new()
    // since we're using signalfd()
    zsys_handler_set(NULL);

    zsock_t *z_command = zsock_new (ZMQ_ROUTER);
    {
	char z_ident[30];
	snprintf(z_ident, sizeof(z_ident), "rtapi_app%d", getpid());
	zsock_set_identity(z_command, z_ident);
	zsock_set_linger(z_command, 1000); // wait for last reply to drain
    }

#ifdef NOTYET
    // determine interface to bind to if remote option set
    if ((remote || z_uri)  && interfaces) {
	char ifname[RTAPI_LINELEN], ip[RTAPI_LINELEN];
	// rtapi_print_msg(RTAPI_MSG_INFO, "rtapi_app: ifpref='%s'\n",interfaces);
	if (parse_interface_prefs(interfaces,  ifname, ip, NULL) == 0) {
	    rtapi_print_msg(RTAPI_MSG_INFO, "rtapi_app: using preferred interface %s/%s\n",
			    ifname, ip);
	    ipaddr = strdup(ip);
	} else {
	    rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_app: INTERFACES='%s'"
			    " - cant determine preferred interface, using %s/%s\n",
			    interfaces, ifname, ipaddr);
	}
	if (z_uri == NULL) { // not given on command line - finalize the URI
	    char uri[RTAPI_LINELEN];
	    snprintf(uri, sizeof(uri), "tcp://%s:*" , ipaddr);
	    z_uri = strdup(uri);
	}

	if ((z_port = zsock_bind(z_command, z_uri)) == -1) {
	    rtapi_print_msg(RTAPI_MSG_ERR,  "cannot bind '%s' - %s\n",
			    z_uri, strerror(errno));
	    global_data->rtapi_app_pid = 0;
	    exit(EXIT_FAILURE);
	} else {
	    z_uri_dsn = zsock_last_endpoint(z_command);
	    rtapi_print_msg(RTAPI_MSG_DBG,  "rtapi_app: command RPC socket on '%s'\n",
			    z_uri_dsn);
	}
    }
#endif
    {	// always bind the IPC socket
	char uri[RTAPI_LINELEN];
	snprintf(uri, sizeof(uri), ZMQIPC_FORMAT,
		 RUNDIR, rtapi_instance_loc, RTAPIMOD, service_uuid);
	mode_t prev = umask(S_IROTH | S_IWOTH | S_IXOTH);
	if ((z_port = zsock_bind(z_command, "%s", uri )) < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,  "cannot bind IPC socket '%s' - %s\n",
			    uri, strerror(errno));
	    global_data->rtapi_app_pid = 0;
	    exit(EXIT_FAILURE);
	}
	rtapi_print_msg(RTAPI_MSG_DBG,"accepting commands at %s\n",uri);
	umask(prev);
    }
    zloop_t *z_loop = zloop_new();
    assert(z_loop);
    zloop_set_verbose(z_loop, z_debug);


    if (trap_signals) {
	zmq_pollitem_t signal_poller = { 0, signal_fd, ZMQ_POLLIN };
	zloop_poller (z_loop, &signal_poller, s_handle_signal, NULL);
    }

    zloop_reader(z_loop, z_command, rtapi_request, NULL);

    zloop_timer (z_loop, BACKGROUND_TIMER, 0, s_handle_timer, NULL);

#ifdef NOTYET
    // no remote rtapi service yet
    if (remote) {
	if (!(av_loop = avahi_czmq_poll_new(z_loop))) {
	    rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_app:%d: zeroconf: "
			    "Failed to create avahi event loop object.",
			    rtapi_instance_loc);
	    return -1;
	} else {
	    char name[255];
	    snprintf(name, sizeof(name), "RTAPI service on %s pid %d", ipaddr, getpid());
	    rtapi_publisher = zeroconf_service_announce(name,
							MACHINEKIT_DNSSD_SERVICE_TYPE,
							RTAPI_DNSSD_SUBTYPE,
							z_port,
							(char *)z_uri_dsn,
							service_uuid,
							process_uuid_str,
							"rtapi", NULL,
							av_loop);
	    if (rtapi_publisher == NULL) {
		rtapi_print_msg(RTAPI_MSG_ERR, "rtapi_app:%d: failed to start zeroconf publisher",
				rtapi_instance_loc);
		return -1;
	    }
	}
    }
#endif

    // report success
    rtapi_print_msg(
        RTAPI_MSG_INFO, "rtapi_app:%d ready flavor=%s gcc=%s git=%s",
        rtapi_instance_loc, (*flavor_name_ptr)(NULL), __VERSION__, GIT_VERSION);

    // the RT stack is now set up and good for use
    global_data->rtapi_app_pid = getpid();

    // main loop
    do {
	retval = zloop_start(z_loop);
    } while  (!(retval || interrupted));

    rtapi_print_msg(RTAPI_MSG_INFO,
		    "exiting mainloop (%s)\n",
		    interrupted ? "interrupted": "by remote command");

    // stop the service announcement
    zeroconf_service_withdraw(rtapi_publisher);

    // shutdown zmq context
    zsock_destroy(&z_command);

    // exiting, so deregister our pid, which will make rtapi_msgd exit too
    global_data->rtapi_app_pid = 0;
    return 0;
}

static int configure_memory(void)
{
    unsigned int i, pagesize;
    char *buf;

    if (use_drivers || (*flavor_feature_ptr)(NULL, FLAVOR_DOES_IO)) {
	// Realtime tweak requires privs
	/* Lock all memory. This includes all current allocations (BSS/data)
	 * and future allocations. */
	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
			    "mlockall() failed: %d '%s'\n",
			    errno,strerror(errno));
	    rtapi_print_msg(RTAPI_MSG_WARN,
			    "For more information, see "
			    "http://wiki.linuxcnc.org/cgi-bin/emcinfo.pl?LockedMemory\n");
	    return 1;
	}
    }

    /* Turn off malloc trimming.*/
    if (!mallopt(M_TRIM_THRESHOLD, -1)) {
	rtapi_print_msg(RTAPI_MSG_WARN,
			"mallopt(M_TRIM_THRESHOLD, -1) failed\n");
	return 1;
    }
    /* Turn off mmap usage. */
    if (!mallopt(M_MMAP_MAX, 0)) {
	rtapi_print_msg(RTAPI_MSG_WARN,
			"mallopt(M_MMAP_MAX, -1) failed\n");
	return 1;
    }
    buf = static_cast<char *>(malloc(PRE_ALLOC_SIZE));
    if (buf == NULL) {
	rtapi_print_msg(RTAPI_MSG_WARN, "malloc(PRE_ALLOC_SIZE) failed\n");
	return 1;
    }
    pagesize = sysconf(_SC_PAGESIZE);
    /* Touch each page in this piece of memory to get it mapped into RAM */
    for (i = 0; i < PRE_ALLOC_SIZE; i += pagesize) {
	/* Each write to this buffer will generate a pagefault.
	 * Once the pagefault is handled a page will be locked in
	 * memory and never given back to the system. */
	buf[i] = 0;
    }
    /* buffer will now be released. As Glibc is configured such that it
     * never gives back memory to the kernel, the memory allocated above is
     * locked for this process. All malloc() and new() calls come from
     * the memory pool reserved and locked above. Issuing free() and
     * delete() does NOT make this locking undone. So, with this locking
     * mechanism we can build C++ applications that will never run into
     * a major/minor pagefault, even with swapping enabled. */
    free(buf);

    return 0;
}

static void
exit_handler(void)
{
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    if ((rusage.ru_majflt - majflt) > 0) {
	// RTAPI already shut down here
	rtapi_print_msg(RTAPI_MSG_WARN,
			"rtapi_app_main %d: %ld page faults, %ld page reclaims\n",
			getpid(), rusage.ru_majflt - majflt,
			rusage.ru_minflt - minflt);
    }
}

static int harden_rt()
{
    // enable core dumps
    struct rlimit core_limit;
    core_limit.rlim_cur = RLIM_INFINITY;
    core_limit.rlim_max = RLIM_INFINITY;

    if (setrlimit(RLIMIT_CORE, &core_limit) < 0)
	rtapi_print_msg(RTAPI_MSG_WARN,
			"setrlimit: %s - core dumps may be truncated or non-existant\n",
			strerror(errno));

    // even when setuid root
    // note this may not be enough
    // echo 1 >
    // might be needed to have setuid programs dump core
    if (prctl(PR_SET_DUMPABLE, 1) < 0)
	rtapi_print_msg(RTAPI_MSG_WARN,
			"prctl(PR_SET_DUMPABLE) failed: no core dumps will be created - %d - %s\n",
			errno, strerror(errno));
    FILE *fd;
    if ((fd = fopen("/proc/sys/fs/suid_dumpable","r")) != NULL) {
	int flag;
	if ((fscanf(fd, "%d", &flag) == 1) && (flag == 0)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
			    "rtapi:%d: cannot create core dumps - /proc/sys/fs/suid_dumpable contains 0",
			    rtapi_instance_loc);
	    rtapi_print_msg(RTAPI_MSG_WARN,
			    "you might have to run 'echo 1 > /proc/sys/fs/suid_dumpable'"
			    " as root to enable rtapi_app core dumps");
	}
	fclose(fd);
    }

    configure_memory();

    if (getrusage(RUSAGE_SELF, &rusage)) {
	rtapi_print_msg(RTAPI_MSG_WARN,
			"getrusage(RUSAGE_SELF) failed: %d '%s'\n",
			errno,strerror(errno));
    } else {
	minflt = rusage.ru_minflt;
	majflt = rusage.ru_majflt;
	if (atexit(exit_handler)) {
	    rtapi_print_msg(RTAPI_MSG_WARN,
			    "atexit() failed: %d '%s'\n",
			    errno,strerror(errno));
	}
    }

#if defined(HAVE_SYS_IO_H)
    // FIXME put this in the module where it belongs!

    // this is a bit of a shotgun approach and should be made more selective
    // however, due to serial invocations of rtapi_app during setup it is not
    // guaranteed the process executing e.g. hal_parport's rtapi_app_main is
    // the same process which starts the RT threads, causing hal_parport
    // thread functions to fail on inb/outb
    if (use_drivers || (*flavor_feature_ptr)(NULL, FLAVOR_DOES_IO)) {
	if (iopl(3) < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "cannot gain I/O privileges - "
			    "forgot 'sudo make setuid'?\n");
	    return -EPERM;
	}
    }
#endif
    return 0;
}


static void usage(int argc, char **argv)
{
    printf("Usage:  %s [options]\n", argv[0]);
}

static struct option long_options[] = {
    {"help",  no_argument,          0, 'h'},
    {"foreground",  no_argument,    0, 'F'},
    {"stderr",  no_argument,        0, 's'},
    {"nosighdlr",   no_argument,    0, 'G'},
    {"instance", required_argument, 0, 'I'},
    {"ini",      required_argument, 0, 'i'},     // default: getenv(INI_FILE_NAME)
    {"drivers",   required_argument, 0, 'D'},
    {"uri",    required_argument,    0, 'U'},
    {"debug", required_argument,    0, 'd'},
    {"svcuuid",   required_argument, 0, 'R'},
    {"interfaces",required_argument, 0, 'n'},
	{"zloopdebug", no_argument,      0, 'z'},
    {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
    int c;
    progname = argv[0];
	char* z_loop_debug = NULL;
    inifile =  getenv("MACHINEKIT_INI");

    uuid_generate_time(process_uuid);
    uuid_unparse(process_uuid, process_uuid_str);

    while (1) {
	int option_index = 0;
	int curind = optind;
	c = getopt_long (argc, argv, "ShH:m:I:f:r:U:NFdR:n:i:sz",
			 long_options, &option_index);
	if (c == -1)
	    break;

	switch (c)	{
	case 'G':
	    trap_signals = false; // ease debugging with gdb
	    break;

	case 'd':
	    debug = atoi(optarg);
	    break;

	case 'D':
	    use_drivers = 1;
	    break;

	case 'F':
	    foreground = 1;
	    break;

	case 'i':
	    inifile = strdup(optarg);
	    break;

	case 'I':
	    rtapi_instance_loc = atoi(optarg);
	    break;

	case 'f':
            strncpy(flavor_name_opt, optarg, MAX_FLAVOR_NAME_LEN-1);
	    break;

	case 'U':
	    z_uri = optarg;
	    break;

	case 'n':
	    interfaces = strdup(optarg);
	    break;

	case 'R':
	    service_uuid = strdup(optarg);
	    break;
	case 'z':
		z_debug = 1;
		break;
	case '?':
	    if (optopt)  fprintf(stderr, "bad short opt '%c'\n", optopt);
	    else  fprintf(stderr, "bad long opt \"%s\"\n", argv[curind]);
	    //usage(argc, argv);
	    exit(1);
	    break;

	default:
	    usage(argc, argv);
	    exit(0);
	}
    }

    if (trap_signals && (getenv("NOSIGHDLR") != NULL))
	trap_signals = false;

    if (inifile && ((inifp = fopen(inifile,"r")) == NULL)) {
	fprintf(stderr,"rtapi_app: cant open inifile '%s'\n", inifile);
    }

    // must have a MKUUID one way or the other
    if (service_uuid == NULL) {
	const char *s;
	if ((s = iniFind(inifp, "MKUUID", "MACHINEKIT"))) {
	    service_uuid = strdup(s);
	}
    }

    if (service_uuid == NULL) {
	fprintf(stderr, "rtapi: no service UUID (-R <uuid> or environment MKUUID) present\n");
	exit(-1);
    }

	z_loop_debug = getenv("ZLOOPDEBUG");
	if(z_loop_debug)
	{
		z_debug = 1;
	}

#ifdef NOTYET
    iniFindInt(inifp, "REMOTE", "MACHINEKIT", &remote);
    if (remote && (interfaces == NULL)) {
	const char *s;
	if ((s = iniFind(inifp, "INTERFACES", "MACHINEKIT"))) {
	    interfaces = strdup(s);
	}
    }
#endif

    // the actual checking for setuid happens in harden_rt() (if needed)
    if (!foreground && (getuid() > 0)) {
	pid_t pid1;
	pid_t pid2;
	int status;
	if ((pid1 = fork())) {
	    waitpid(pid1, &status, 0);
	    exit(status);
	} else if (!pid1) {
	    if ((pid2 = fork())) {
		exit(0);
	    } else if (!pid2) {
		setsid(); // Detach from the parent session
	    } else {
		exit(1);
	    }
	} else {
	    exit(1);
	}
    } else {
	// dont run as root XXX
    }
    exit(mainloop(argc, argv));
}

static void remove_module(std::string name)
{
    std::vector<string>::iterator invalid;
    invalid = remove( loading_order.begin(), loading_order.end(), name );
}

// instparams are set on each instantiation, but must be set
// to default values before applying new instance params
// because the old ones will remain in place, overriding defaults.
//
// the basic idea is:
// once a module is loaded, it is scanned for instance parameter defaults
// as stored in the .rtapi_export section.
//
// those are retrieved, and recorded in the per-module modinfo
// in the same format as received via zeromq/protobuf from halcmd/cython.
//
// in do_newinst_cmd(), apply those defaults before the actual parameters
// are applied.
static int record_instparms(Module &mi)
{
    void *section = NULL;
    int csize = -1;
    size_t i;
    vector<string> tokens;
    string pn;

    csize = mi.elf_section(".rtapi_export" , &section);
    if (csize < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "Can't open %s\n", mi.path().c_str());
	return -1;
    }

    char *s;
    vector<string> symbols;
    string sym;
    for (s = (char *)section;
	 s < ((char *)section + csize);
	 s += strlen(s) + 1)
	if (strlen(s))
	    symbols.push_back(string(s));

    // walk the symbols, and extract the instparam names.
    string pat(RTAPI_IP_SYMPREFIX "address_");
    vector<string> instparms;
    pbstringarray_t iparm;
    iparms[mi.name] = iparm;

    for (i = 0; i < symbols.size(); i++) {
	string ip(symbols[i]);
	if (ip.rfind(pat) == 0) {
            ip.replace(0, pat.length(), "");
	    char **type = mi.sym<char**>(RTAPI_IP_SYMPREFIX "type_" + ip);
	    void *addr = mi.sym<void*>(RTAPI_IP_SYMPREFIX "address_" + ip);
	    // char **desc =
            //     mi.sym<char**>(RTAPI_IP_SYMPREFIX "description_" + ip);
	    int i;
	    unsigned u;
	    long l;
	    char *s;
	    char buffer[100];
	    string tmp;
	    if (strlen(*type) == 1) {
		switch (**type) {
		case 'i':
		    i = **((int **) addr);
		    snprintf(buffer, sizeof(buffer),"%s=%d", ip.c_str(),i);
		    *iparm.Add() = buffer;
		    break;
		case 'u':
		    u = **((unsigned **) addr);
		    snprintf(buffer, sizeof(buffer),"%s=%u", ip.c_str(),u);
		    *iparm.Add() = buffer;
		    break;
		case 'l':
		    l = **((long **) addr);
		    snprintf(buffer, sizeof(buffer),"%s=%ld", ip.c_str(),l);
		    *iparm.Add() = buffer;
		    break;
		case 's':
		    s = **(char ***) addr;
		    snprintf(buffer, sizeof(buffer),"%s=\"%s\"", ip.c_str(),s);
		    *iparm.Add() = buffer;
		    break;
		default:
		    rtapi_print_msg(RTAPI_MSG_ERR,
				    "%s: unhandled instance param type '%c'",
				    mi.name.c_str(), **type);
		}
	    } else {
		// TBD: arrays
	    }
	    // rtapi_print_msg(RTAPI_MSG_INFO,
	    // 		    "--inst param '%s' at %p type='%s' descr='%s'",
	    // 		    ip.c_str(), addr,*type, *desc);
	}

    }
    rtapi_print_msg(RTAPI_MSG_DBG,
		    "%s default iparms: '%s'", mi.name.c_str(),
		    pbconcat(iparm).c_str());
    free(section);
    return 0;
}
