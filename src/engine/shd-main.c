/*
 * The Shadow Simulator
 * Copyright (c) 2010-2011, Rob Jansen
 * See LICENSE for licensing information
 */

#if 0 /* these are only avail in glib >= 2.30, needed for signals */
#include <glib-unix.h>
#endif

#include "shadow.h"

Engine* shadow_engine;

gint shadow_main(gint argc, gchar* argv[]) {
    /* check the compiled GLib version */
    if (!GLIB_CHECK_VERSION(2, 32, 0)) {
	    g_printerr("** GLib version 2.32.0 or above is required but Shadow was compiled against version %u.%u.%u\n",
		    (guint)GLIB_MAJOR_VERSION, (guint)GLIB_MINOR_VERSION, (guint)GLIB_MICRO_VERSION);
	    return -1;
    }

    /* check the that run-time GLib matches the compiled version */
    const gchar* mismatch = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    if(mismatch) {
	    g_printerr("** The version of the run-time GLib library (%u.%u.%u) is not compatible with the version against which Shadow was compiled (%u.%u.%u). GLib message: '%s'\n",
        glib_major_version, glib_minor_version, glib_micro_version,
        (guint)GLIB_MAJOR_VERSION, (guint)GLIB_MINOR_VERSION, (guint)GLIB_MICRO_VERSION,
        mismatch);
	    return -1;
    }

	/* setup configuration - this fails and aborts if invalid */
	Configuration* config = configuration_new(argc, argv);
	if(!config) {
		/* incorrect options given */
		return -1;
	} else if(config->printSoftwareVersion) {
		g_printerr("Shadow v%s\nFor more information, visit http://shadow.github.io or https://github.com/shadow\n", SHADOW_VERSION);
		configuration_free(config);
		return 0;
	}

	/* we better have preloaded libshadow_preload.so */
	const gchar* ldPreloadValue = g_getenv("LD_PRELOAD");
	if(!ldPreloadValue || !g_strstr_len(ldPreloadValue, -1, "libshadow-preload.so")) {
		g_printerr("** Environment Check Failed: LD_PRELOAD does not contain libshadow-preload.so\n");
		return -1;
	}

	/* allocate our driving application structure */
	shadow_engine = engine_new(config);
	/* our first call to create the worker for the main thread */
	Worker* mainThreadWorker = worker_getPrivate();
	/* make the engine available */
	mainThreadWorker->cached_engine = shadow_engine;

	/* check if we were able to read the CPU freq file */
	if(engine_getRawCPUFrequency(shadow_engine) == 0) {
		info("unable to read '%s' for copying", CONFIG_CPU_MAX_FREQ_FILE);
	}

	/* hook in our logging system. stack variable used to avoid errors
	 * during cleanup below. */
	GLogLevelFlags configuredLogLevel = configuration_getLogLevel(config);
	g_log_set_default_handler(logging_handleLog, &(configuredLogLevel));

    GDateTime* dt_now = g_date_time_new_now_local();
    gchar* dt_format = g_date_time_format(dt_now, "%F %H:%M:%S");
    message("Shadow v%s initialized at %s using GLib v%u.%u.%u",
        SHADOW_VERSION, dt_format, (guint)GLIB_MAJOR_VERSION, (guint)GLIB_MINOR_VERSION, (guint)GLIB_MICRO_VERSION);
    g_date_time_unref(dt_now);
    g_free(dt_format);

#if 0 /* @todo: these are only avail in glib >= 2.30 */
	/* setup signal handlers for gracefully handling shutdowns */
	g_unix_signal_add(SIGTERM, engine_handleInterruptSignal, shadow_engine);
	g_unix_signal_add(SIGHUP, engine_handleInterruptSignal, shadow_engine);
	g_unix_signal_add(SIGINT, engine_handleInterruptSignal, shadow_engine);
#endif

	/* store parsed actions from each user-configured simulation script  */
	GQueue* actions = g_queue_new();
	Parser* xmlParser = parser_new();

	/* parse built-in examples, or input files */
	gboolean success = TRUE;
	if(config->runEchoExample) {
		GString* echo = example_getEchoExampleContents();
		success = parser_parseContents(xmlParser, echo->str, echo->len, actions);
		g_string_free(echo, TRUE);
	} else if(config->runFileExample) {
		GString* file = example_getFileExampleContents();
		success = parser_parseContents(xmlParser, file->str, file->len, actions);
		g_string_free(file, TRUE);
	} else if(config->runTorrentExample) {
		GString* torrent = example_getTorrentExampleContents();
		success = parser_parseContents(xmlParser, torrent->str, torrent->len, actions);
		g_string_free(torrent, TRUE);
	} else if (config->runBrowserExample) {
		GString* browser = example_getBrowserExampleContents();
		success = parser_parseContents(xmlParser, browser->str, browser->len, actions);
		g_string_free(browser, TRUE);
	} else {
		/* parse all given input XML files */
		while(success && g_queue_get_length(config->inputXMLFilenames) > 0) {
			GString* filename = g_queue_pop_head(config->inputXMLFilenames);
			success = parser_parseFile(xmlParser, filename, actions);
		}
	}

	parser_free(xmlParser);

	/* if there was an error parsing, bounce out */
	if(success) {
		message("successfully parsed Shadow XML input!");
	} else {
		g_queue_free(actions);
		return -1;
	}

	/*
	 * loop through actions that were created from parsing. this will create
	 * all the nodes, networks, applications, etc., and add an application
	 * start event for each node to bootstrap the simulation. Note that the
	 * plug-in libraries themselves are not loaded until a worker needs it,
	 * since each worker will need its own private version.
	 */
	while(g_queue_get_length(actions) > 0) {
		Action* a = g_queue_pop_head(actions);
		runnable_run(a);
		runnable_free(a);
	}
	g_queue_free(actions);

	/* run the engine to drive the simulation. when this returns, we are done */
	gint n = config->nWorkerThreads;
	debug("starting %i-threaded engine (main + %i workers)", (n + 1), n);
	gint retval = engine_run(shadow_engine);
	debug("engine finished, cleaning up...");

	/* cleanup */
	engine_free(shadow_engine);
	shadow_engine = NULL;
	worker_free(mainThreadWorker);
	configuration_free(config);

	return retval;
}
