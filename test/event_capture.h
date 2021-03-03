#ifndef EVENT_CAPTURE_H
#define EVENT_CAPTURE_H

#include <functional>
#include <Poco/Thread.h>
#include <Poco/Event.h>
#include <Poco/RunnableAdapter.h>
#include <unistd.h>
#include <sinsp.h>
#include <analyzer.h>
#include <gtest.h>
#include <inttypes.h>
#include "protocol.h"
#include "dragent_message_queues.h"

class callback_param
{
public:
	sinsp_evt *m_evt;
	sinsp *m_inspector;
	sinsp_analyzer* m_analyzer;
};

typedef std::function<void (sinsp *inspector, sinsp_analyzer* analyzer)> before_open_t;
typedef std::function<void (sinsp *inspector, sinsp_analyzer* analyzer)> before_close_t;
typedef std::function<bool (sinsp_evt *evt) > event_filter_t;
typedef std::function<void (const callback_param &param) > captured_event_callback_t;

// Returns true/false to indicate whether the capture should continue
// or stop
typedef std::function<bool () > capture_continue_t;

typedef std::function<void (sinsp* inspector) > run_callback_t;

class event_capture
{
public:

	void capture();

	void stop_capture()
	{
		m_stopped = true;
	}

	void wait_for_capture_start()
	{
		m_capture_started.wait();
	}

	void wait_for_capture_stop()
	{
		m_capture_stopped.wait();
	}

	static void do_nothing(sinsp *inspector, sinsp_analyzer* analyzer)
	{
	}

	static bool always_continue()
	{
		return true;
	}

	static void run(run_callback_t run_function,
			captured_event_callback_t captured_event_callback,
			event_filter_t filter, before_open_t before_open)
	{
		sinsp_configuration configuration;
		run(run_function,
		    captured_event_callback,
		    filter,
		    configuration,
		    131072,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		    SCAP_MODE_LIVE,
		    before_open);
	}

	static void run(run_callback_t run_function,
	                captured_event_callback_t captured_event_callback,
	                event_filter_t filter,
			before_open_t before_open,
			before_close_t before_close)
	{
		sinsp_configuration configuration;
		run(run_function,
		    captured_event_callback,
		    filter,
		    configuration,
		    131072,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		    SCAP_MODE_LIVE,
		    before_open,
		    before_close);
	}

	static void run(run_callback_t run_function,
	                captured_event_callback_t captured_event_callback,
	                event_filter_t filter)
	{
		sinsp_configuration configuration;
		run(run_function, captured_event_callback, filter, configuration);
	}

	static void run(run_callback_t run_function,
	                captured_event_callback_t captured_event_callback)
	{
		event_filter_t no_filter = [](sinsp_evt *)
		{
			return true;
		};
		sinsp_configuration configuration;
		run(run_function, captured_event_callback, no_filter, configuration);
	}

	static void run_nodriver(run_callback_t run_function,
				 captured_event_callback_t captured_event_callback)
	{
		event_filter_t no_filter = [](sinsp_evt *)
		{
			return true;
		};

		sinsp_configuration configuration;
		run(run_function,
		    captured_event_callback,
		    no_filter,
		    configuration,
		    131072,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		     (uint64_t)60 * 1000 * 1000 * 1000,
		    SCAP_MODE_NODRIVER);
	}

	static void run(
		run_callback_t run_function,
		captured_event_callback_t captured_event_callback,
		event_filter_t filter,
		const sinsp_configuration& configuration,
		uint32_t max_thread_table_size = 131072,
		uint64_t thread_timeout_ns =  (uint64_t)60 * 1000 * 1000 * 1000,
		uint64_t inactive_thread_scan_time_ns =  (uint64_t)60 * 1000 * 1000 * 1000,
		scap_mode_t mode = SCAP_MODE_LIVE,
		before_open_t before_open = event_capture::do_nothing,
		before_close_t before_close = event_capture::do_nothing,
		capture_continue_t capture_continue = event_capture::always_continue,
		uint64_t max_timeouts = 3)
	{
		event_capture capturing;
		capturing.m_mode = mode;
		capturing.m_captured_event_callback = captured_event_callback;
		capturing.m_before_open = before_open;
		capturing.m_before_close = before_close;
		capturing.m_capture_continue = capture_continue;
		capturing.m_filter = filter;
		capturing.m_configuration = configuration;
		capturing.m_max_thread_table_size = max_thread_table_size;
		capturing.m_thread_timeout_ns = thread_timeout_ns;
		capturing.m_inactive_thread_scan_time_ns = inactive_thread_scan_time_ns;
		capturing.m_max_timeouts = max_timeouts;

		Poco::RunnableAdapter<event_capture> runnable(capturing, &event_capture::capture);
		Poco::Thread thread;
		thread.start(runnable);
		capturing.wait_for_capture_start();

		if(!capturing.m_start_failed)
		{
			run_function(capturing.m_inspector);
			capturing.stop_capture();
			capturing.wait_for_capture_stop();
			//    capturing.re_read_dump_file();
		}
		else
		{
			GTEST_MESSAGE_(capturing.m_start_failure_message.c_str(), ::testing::TestPartResult::kFatalFailure);
		}

		thread.join();
	}

private:
	event_capture() : m_stopped(false),
	                  m_start_failed(false),
	                  m_flush_queue(1000),
	                  m_transmit_queue(1000)
	{
	}

	void re_read_dump_file()
	{
		try
		{
			sinsp inspector;
			sinsp_evt *event;

			inspector.open(m_dump_filename);
			uint32_t res;
			do
			{
				res = inspector.next(&event);
			}
			while(res == SCAP_SUCCESS);
			ASSERT_EQ((int)SCAP_EOF, (int)res);
		}
		catch(sinsp_exception &e)
		{
			FAIL() << "caught exception " << e.what();
		}
	}

	bool handle_event(sinsp_evt *event)
	{
		if(::testing::Test::HasNonfatalFailure())
		{
			return true;
		}
		bool res = true;
		if(m_filter(event))
		{
			try
			{
				m_param.m_evt = event;
				m_captured_event_callback(m_param);
			}
			catch(...)
			{
				res = false;
			}
		}
		if(!m_capture_continue())
		{
			return false;
		}
		if(!res || ::testing::Test::HasNonfatalFailure())
		{
			std::cerr << "failed on event " << event->get_num() << std::endl;
		}
		return res;
	}

	Poco::Event m_capture_started;
	Poco::Event m_capture_stopped;
	bool m_stopped;
	event_filter_t m_filter;
	captured_event_callback_t m_captured_event_callback;
	before_open_t m_before_open;
	before_close_t m_before_close;
	capture_continue_t m_capture_continue;
	sinsp_configuration m_configuration;
	uint32_t m_max_thread_table_size;
	uint64_t m_thread_timeout_ns;
	uint64_t m_inactive_thread_scan_time_ns;
	bool m_start_failed;
	std::string m_start_failure_message;
	std::string m_dump_filename;
	callback_param m_param;
	sinsp* m_inspector;
	sinsp_analyzer* m_analyzer;
	scap_mode_t m_mode;
	uint64_t m_max_timeouts;
	sinsp_analyzer::flush_queue m_flush_queue;
	protocol_queue m_transmit_queue;
};


#endif  /* EVENT_CAPTURE_H */

