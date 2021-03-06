/*
 * Tlf - contest logging program for amateur radio operators
 * Copyright (C) 2014-2002-2003 Rein Couperus <pa0rct@amsat.org>
 *               2011, 2014, 2016 Thomas Beierlein <tb@forth-ev.de>
 *               2014, 2016       Ervin Hegedus <airween@gmail.com>
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

#include "startmsg.h"
#include <stdlib.h>	// need for abs()
#include <string.h>
#include <stdarg.h>	// need for va_list...
#include <unistd.h>
#include <pthread.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "fldigixmlrpc.h"

#ifdef HAVE_LIBXMLRPC
# include <xmlrpc-c/base.h>
# include <xmlrpc-c/client.h>
# include <xmlrpc-c/client_global.h>
#endif

#ifdef HAVE_LIBHAMLIB
# include <hamlib/rig.h>
#endif

#define NAME "Tlf"
#define XMLRPCVERSION "1.0"

typedef struct xmlrpc_res_s {
    int			intval;
    const char		*stringval;
    const unsigned char	*byteval;
} xmlrpc_res;

#define CENTER_FREQ 2210	/* low: 2125Hz, high: 2295Hz, shift: 170Hz,
				    center: 2125+(170/2) = 2210Hz */
#define MAXSHIFT 20		/* max shift value in Fldigi, when Tlf set
				   it back to RIG carrier */

#ifdef HAVE_LIBHAMLIB
extern RIG *my_rig;
#endif

extern char fldigi_url[50];

int fldigi_var_carrier = 0;
int fldigi_var_shift_freq = 0;
static int initialized = 0;

pthread_mutex_t xmlrpc_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Used XML RPC methods, and its formats of arguments
 * ==================================================
 main.rx             n:n  - RX
 main.tx             n:n  - TX
 main.get_trx_state  s:n  - get RX/TX state, 's' could be "RX" | "TX"
   rx.get_data       6:n   (bytes:) - get content of RX window since last query
 text.add_tx         n:s  - add content to TX window
modem.get_carrier    i:n  - get carrier of modem
modem.set_carrier    i:i  - set carrier of modem


 // other usable functions
 text.get_rx_length  i:n  - get length of content of RX window
 text.get_rx         6:ii - (bytes:int|int) - get part content of RX window
			    [start:length]
*/

#ifdef HAVE_LIBXMLRPC
xmlrpc_env env;
xmlrpc_server_info * serverInfoP = NULL;
#endif


int fldigi_xmlrpc_init() {
#ifdef HAVE_LIBXMLRPC
    xmlrpc_client_init2(&env, XMLRPC_CLIENT_NO_FLAGS, NAME,
	    XMLRPCVERSION, NULL, 0);
    serverInfoP = xmlrpc_server_info_new(&env, fldigi_url);
    if (env.fault_occurred != 0) {
	serverInfoP = NULL;
	initialized = 0;
	return -1;
    }

    initialized = 1;
#endif
    return 0;
}

int fldigi_xmlrpc_cleanup() {
#ifdef HAVE_LIBXMLRPC
    if (serverInfoP != NULL) {
	xmlrpc_server_info_free(serverInfoP);
	serverInfoP = NULL;
	initialized = 0;
    }
#endif
    return 0;
}

#ifdef HAVE_LIBXMLRPC
int fldigi_xmlrpc_query(xmlrpc_res * local_result, xmlrpc_env * local_env,
			char * methodname, char * format, ...) {
    static int connerr = 0;
    static unsigned int connerrcnt = 0;
    xmlrpc_value * callresult;
    xmlrpc_value * pcall_array = NULL;
    xmlrpc_value * va_param = NULL;
    va_list argptr;
    int restype;
    size_t bytesize = 0;


    if (initialized == 0) {
	return -1;
    }

    pthread_mutex_lock( &xmlrpc_mutex );
    /*
    if connerr had been set up to 1, that means an error
    occured at last xmlrpc_call() method
    if that true, then we count the number of calling this
    function (xmlrpc()), if counter reaches 10, then clear
    it, and try again
    this handles the xmlrpc_call() errors, eg. Fldigi is
    unreacheable, but it will check again and again, not
    need to restart Tlf
    */
    if (connerr == 1) {
	if (connerrcnt == 10) {
	    connerr = 0;
	    connerrcnt = 0;
	}
	else {
	    connerrcnt++;
	}
    }

    local_result->stringval = NULL;
    local_result->byteval = NULL;

    if (connerr == 0) {
	va_start(argptr, format);

	xmlrpc_env_init(local_env);
	pcall_array = xmlrpc_array_new(local_env);
	while(*format != '\0') {
	    if(*format == 's') {
		char* s = va_arg(argptr, char *);
		va_param = xmlrpc_string_new(local_env, s);
		if (local_env->fault_occurred) {
		    va_end(argptr);
		    pthread_mutex_unlock( &xmlrpc_mutex );
		    return -1;
		}
		xmlrpc_array_append_item(local_env, pcall_array, va_param);
		if (local_env->fault_occurred) {
		    va_end(argptr);
		    pthread_mutex_unlock( &xmlrpc_mutex );
		    return -1;
		}
		xmlrpc_DECREF(va_param);
	    }
	    else if(*format == 'd') {
		int d = va_arg(argptr, int);
		va_param = xmlrpc_int_new(local_env, d);
		xmlrpc_array_append_item(local_env, pcall_array, va_param);
		if (local_env->fault_occurred) {
		    va_end(argptr);
		    pthread_mutex_unlock( &xmlrpc_mutex );
		    return -1;
		}
		xmlrpc_DECREF(va_param);
	    }
	    format++;
	}

	va_end(argptr);

	callresult = xmlrpc_client_call_server_params(local_env, serverInfoP,
		methodname, pcall_array);
	if (local_env->fault_occurred) {
	    // error till xmlrpc_call
	    connerr = 1;
	    xmlrpc_DECREF(pcall_array);
	    xmlrpc_env_clean(local_env);
	    pthread_mutex_unlock( &xmlrpc_mutex );
	    return -1;
	}
	else {
	    restype = xmlrpc_value_type(callresult);
	    if (restype == 0xDEAD) {
		xmlrpc_DECREF(callresult);
		xmlrpc_DECREF(pcall_array);
		xmlrpc_env_clean(local_env);
		pthread_mutex_unlock( &xmlrpc_mutex );
		return -1;
	    }
	    else {
		local_result->intval = 0;
	    }
	    switch(restype) {
		// int
		case XMLRPC_TYPE_INT:
		    xmlrpc_read_int(local_env, callresult,
			    &local_result->intval);
		    break;
		// string
		case XMLRPC_TYPE_STRING:
		    xmlrpc_read_string(local_env, callresult,
			    &local_result->stringval);
		    break;
		// byte stream
		case XMLRPC_TYPE_BASE64:
		    xmlrpc_read_base64(local_env, callresult,
			    &bytesize, &local_result->byteval);
		    local_result->intval = (int)bytesize;
		    break;
	    }
	    xmlrpc_DECREF(callresult);
	}

	xmlrpc_DECREF(pcall_array);
    }
    if (connerr == 0) {
	pthread_mutex_unlock( &xmlrpc_mutex );
	return 0;
    }
    else {
	pthread_mutex_unlock( &xmlrpc_mutex );
	return -1;
    }
}
#endif

/* command fldigi to RX now */
void fldigi_to_rx() {
#ifdef HAVE_LIBXMLRPC
    xmlrpc_res result;
    xmlrpc_env env;
    fldigi_xmlrpc_query(&result, &env, "main.rx", "");
    if (result.stringval != NULL) {
	free((void *)result.stringval);
    }
#endif
}

int fldigi_send_text(char * line) {
    int rc = 0;

#ifdef HAVE_LIBXMLRPC
    xmlrpc_res result;
    xmlrpc_env env;

    rc = fldigi_xmlrpc_query(&result, &env, "main.get_trx_state", "");
    if (rc != 0) {
	return -1;
    }

    if (strcmp(result.stringval, "TX") == 0) {
	free((void *)result.stringval);
	rc = fldigi_xmlrpc_query(&result, &env, "main.rx", "");
	if (rc != 0) {
	    return -1;
	}
	rc = fldigi_xmlrpc_query(&result, &env, "text.clear_tx", "");
	if (rc != 0) {
	    return -1;
	}
	sleep(2);
    }
    if (result.stringval != NULL) {
	free((void *)result.stringval);
    }

    rc = fldigi_xmlrpc_query(&result, &env, "text.add_tx", "s", line);
    if (rc != 0) {
	return -1;
    }
    /* switch to rx afterwards */
    rc = fldigi_xmlrpc_query(&result, &env, "text.add_tx", "s", "^r");
    if (rc != 0) {
	return -1;
    }

    if (result.stringval != NULL) {
	free((void *)result.stringval);
    }
    /* switch to tx */
    rc = fldigi_xmlrpc_query(&result, &env, "main.tx", "");

    if (result.stringval != NULL) {
	free((void *)result.stringval);
    }
#endif
    return rc;
}


int fldigi_get_rx_text(char * line) {
#ifdef HAVE_LIBXMLRPC
    int rc;
    xmlrpc_res result;
    xmlrpc_env env;
    static int lastpos = 0;
    int textlen = 0;
    int retval = 0;

    rc = fldigi_xmlrpc_query(&result, &env, "text.get_rx_length", "");
    if (rc != 0) {
	return 0;
    }

    textlen = result.intval;
    if (lastpos == 0) {
	lastpos = textlen;
    }
    else {
	if (lastpos < textlen) {
	    rc = fldigi_xmlrpc_query(&result, &env, "text.get_rx", "dd",
		    lastpos, textlen);
	    if (rc != 0) {
		return 0;
	    }

	    if (result.intval > 0 && result.byteval != NULL) {
		memcpy(line, result.byteval, result.intval);
		line[result.intval] = '\0';
		retval = result.intval;
	    }
	    if (result.byteval != NULL) {
		free((void *)result.byteval);
	    }
	}
    }
    lastpos = textlen;
    return retval;

#else
    return 0;
#endif

}


int fldigi_xmlrpc_get_carrier() {

#ifndef HAVE_LIBXMLRPC
    return 0;
#else

    extern int rigmode;
    extern int trx_control;
    int rc;
    xmlrpc_res result;
    xmlrpc_env env;
    int signum;
    int modeshift;

    rc = fldigi_xmlrpc_query(&result, &env, "modem.get_carrier", "");
    if (rc != 0) {
	return -1;
    }

    fldigi_var_carrier = (int)result.intval;

#ifdef HAVE_LIBHAMLIB
    if (trx_control > 0) {
	if (rigmode == RIG_MODE_RTTY || rigmode == RIG_MODE_RTTYR) {
	    if (fldigi_var_carrier != CENTER_FREQ &&
		    abs(CENTER_FREQ - fldigi_var_carrier) > MAXSHIFT) {
		if (fldigi_var_shift_freq == 0) {
		    rc = fldigi_xmlrpc_query(&result, &env,
			    "modem.set_carrier", "d",
			    (xmlrpc_int32) CENTER_FREQ);
		    if (rc != 0) {
			return 0;
		    }
		    fldigi_var_shift_freq = CENTER_FREQ-fldigi_var_carrier;
		}
	    }
	}

	if (rigmode != RIG_MODE_NONE) {
	    switch (rigmode) {
		case RIG_MODE_USB:	signum = 1;
					modeshift = 0;
					break;
		case RIG_MODE_LSB:	signum = -1;
					modeshift = 0;
					break;
		case RIG_MODE_RTTY:	signum = 0;
					modeshift = -100; // on my TS850, in FSK mode, the QRG is differ by 100Hz up
							  // possible need to check in other rigs
					modeshift = 0;
					break;
		case RIG_MODE_RTTYR:	signum = 0;	// not checked - I don't have RTTY-REV mode on my RIG
					modeshift = -100;
					break;
		default:		signum = 0;	// this is the "normal"
					modeshift = 0;
	    }
	    fldigi_var_carrier = ((signum)*fldigi_var_carrier)+modeshift;
	}
    }
#endif
    return 0;
#endif

}

int fldigi_get_carrier() {
#ifdef HAVE_LIBXMLRPC
        return fldigi_var_carrier;
#else
        return 0;
#endif
}

int fldigi_get_shift_freq() {
#ifdef HAVE_LIBXMLRPC
	int t;				/* temp var to store real variable
					   before cleaning it up */
	t = fldigi_var_shift_freq;	/* clean is necessary to check that
					   it readed by called this function */
	fldigi_var_shift_freq = 0;	/* needs to keep in sync with the
					   rig VFO */
        return t;
#else
        return 0;
#endif
}

void xmlrpc_showinfo() {
#ifdef HAVE_LIBXMLRPC		// Show xmlrpc status
    showmsg("XMLRPC compiled in");
#else
    showmsg("XMLRPC NOT compiled");
#endif
}
