/*
    httperf -- a tool for measuring web server performance
    Copyright 2000-2007 Hewlett-Packard Company

    This file is part of httperf, a web server performance measurment
    tool.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.
    
    In addition, as a special exception, the copyright holders give
    permission to link the code of this work with the OpenSSL project's
    "OpenSSL" library (or with modified versions of it that use the same
    license as the "OpenSSL" library), and distribute linked combinations
    including the two.  You must obey the GNU General Public License in
    all respects for all of the code used other than "OpenSSL".  If you
    modify this file, you may extend this exception to your version of the
    file, but you are not obligated to do so.  If you do not wish to do
    so, delete this exception statement from your version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  
    02110-1301, USA
*/

/* Cwmp statistics collector.  */

#include "config.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <generic_types.h>

#include <object.h>
#include <timer.h>
#include <httperf.h>
#include <call.h>
#include <conn.h>
#include <localevent.h>
#include <session.h>
#include <stats.h>
#include <time.h>
#include <cwmp.h>

static struct
  {
    u_int num_rate_samples;
    u_int num_succeeded_since_last_sample;
    Time rate_sum;
    Time rate_sum2;
    Time rate_min;
    Time rate_max;

    u_int num_completed;
    u_int num_succeeded;
    Time lifetime_sum;

    u_int num_err_workflow;
    u_int num_err_bad_req;
    u_int num_err_no_resq;
    u_int num_err_others;

    u_int num_inform_completed;
    size_t req_bytes_sent;
  }
st;

#define CWMP_STAT_SESS_PRIVATE_DATA(c)						\
  ((Cwmp_Stat_Sess_Private_Data *) ((char *)(c) + cwmp_stat_sess_private_data_offset))

#define DEFAULT_SCREEN_WIDTH     80 /* How wide we assume the screen is if termcap fails. */
#define PERCENT_FORMAT_LENGTH    4  /* The maximum number of percent value format can ever yield */
#define WHITESPACE_LENGTH        3  /* Amount of screen width taken up by whitespace for each bar */
#define BAR_BORDER_WIDTH         2  /* The amount of width taken up by the border of the bar component */

typedef struct Cwmp_Stat_Sess_Private_Data
  {
    Time birth_time;		/* when this session got created */
  }
Cwmp_Stat_Sess_Private_Data;

static size_t cwmp_stat_sess_private_data_offset = -1;
extern size_t cwmp_sess_private_data_offset;
extern u_int cwmp_num_sessions_generated;

static unsigned int 
get_screen_width (void)
{
  struct winsize w;
  
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) < 0)
  {
    return DEFAULT_SCREEN_WIDTH;
  }
  
  return w.ws_col;
}

static void
perf_sample (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Time weight = call_arg.d;
  double rate;

  assert (et == EV_PERF_SAMPLE);

  rate = weight*st.num_succeeded_since_last_sample;
  st.num_succeeded_since_last_sample = 0;

  if (verbose)
    printf ("session-rate = %-8.1f\n", rate);

  ++st.num_rate_samples;
  st.rate_sum += rate;
  st.rate_sum2 += SQUARE (rate);
  if (rate < st.rate_min)
    st.rate_min = rate;
  if (rate > st.rate_max)
    st.rate_max = rate;
}

static void
increase_bar (int value, int max, int bar_width)
{
  int i, cur_width;

  if (value > max)
  {
     value = max;
  }

  cur_width = value * bar_width / max;

  putchar ('[');

  for (i = 0; i < cur_width; i ++)
  {
    putchar ('|');
  }
  
  for (i = cur_width; i < bar_width; i ++)
  {
    putchar (' ');
  }

  printf ("] %3d%%  ", value * 100 / max);
}

static void
process_bar_print (void)
{
  static int first_time = 0;
  int screen_width = get_screen_width();
  int colum_width = screen_width / 2;
  int bar_width = colum_width  - BAR_BORDER_WIDTH - PERCENT_FORMAT_LENGTH - WHITESPACE_LENGTH;  
  int num_finished = st.num_succeeded+ st.num_err_workflow + st.num_err_bad_req + st.num_err_no_resq + st.num_err_others;

  if (0 == first_time)
  {
    first_time = 1;
    printf ("\n%-*s%-*s\n", colum_width, "Inform completed", colum_width, "Session completed");
  }
  
  putchar ('\r');

  increase_bar (st.num_inform_completed, param.cwmp.num_sessions, bar_width);
  increase_bar (num_finished, param.cwmp.num_sessions, bar_width);
  fflush (stdout);

  if (param.cwmp.num_sessions == num_finished)
  {
     printf ("\n\n");
  }
}

static void
sess_created (Event_Type et, Object *obj, Any_Type regarg, Any_Type callarg)
{
  Cwmp_Stat_Sess_Private_Data *stat_priv;
  Sess *sess;

  assert (et == EV_SESS_NEW && object_is_sess (obj));
  sess = (Sess *) obj;
  stat_priv = CWMP_STAT_SESS_PRIVATE_DATA (sess);
  stat_priv->birth_time = timer_now ();
}

static void
sess_destroyed (Event_Type et, Object *obj, Any_Type regarg, Any_Type callarg)
{
  size_t old_size, new_size;
  Cwmp_Stat_Sess_Private_Data *stat_priv;
  Cwmp_Sess_Private_Data *cwmp_priv;
  Sess *sess;
  Time delta, now = timer_now ();

  assert (et == EV_SESS_DESTROYED && object_is_sess (obj));
  sess = (Sess *) obj;
  stat_priv = CWMP_STAT_SESS_PRIVATE_DATA (sess);  
  cwmp_priv = CWMP_SESS_PRIVATE_DATA (sess);

  delta = (now - stat_priv->birth_time);

  switch (cwmp_priv->cwmp_result)
  {
    case CWMP_ERR_WORKFLOW:
      ++st.num_err_workflow;
      break;
      
    case CWMP_ERR_BAD_REQ:
      ++st.num_err_bad_req;
      break;

    case CWMP_ERR_NO_RESP:
      ++st.num_err_no_resq;
      break;
      
    case CWMP_ERR_OTHERS:
      ++st.num_err_others;
      break;
      
    default:
      break;
  }
  
  if (0 == sess->failed)
  {
    ++st.num_succeeded_since_last_sample;
    ++st.num_completed;
    st.lifetime_sum += delta;

    if (CWMP_ERR_NONE == cwmp_priv->cwmp_result)
    {
      ++st.num_succeeded;
    }
  }

  if (verbose && 0 == param.forever)
  {
    process_bar_print();
  }
}

static void
call_send_start (Event_Type et, Object *obj, Any_Type regarg, Any_Type callarg)
{
  Cwmp_Stat_Sess_Private_Data *stat_priv;
  Cwmp_Sess_Private_Data *cwmp_priv;
  Sess *sess;
  Call *call;  

  assert (et == EV_CALL_SEND_START && object_is_sess (obj));
  call = (Call *) obj;
  sess = session_get_sess_from_call (call);
  stat_priv = CWMP_STAT_SESS_PRIVATE_DATA (sess);
  cwmp_priv = CWMP_SESS_PRIVATE_DATA (sess);

  if (CPE_INFORM_DONE == cwmp_priv->current_cpe_action)
  {
    st.num_inform_completed++;
  }

  if (verbose && 0 == param.forever)
  {
    process_bar_print();
  }
}

static void
send_stop(Event_Type et, Object * obj, Any_Type reg_arg, Any_Type call_arg)
{
  Call *c = (Call *) obj;

  assert(et == EV_CALL_SEND_STOP && object_is_call(c));

  st.req_bytes_sent += c->req.size;
}

static void
init (void)
{
  Any_Type arg;
  size_t size;

  cwmp_stat_sess_private_data_offset = object_expand (OBJ_SESS,
					    sizeof (Cwmp_Stat_Sess_Private_Data));

  st.rate_min = DBL_MAX;
  
  arg.l = 0;
  event_register_handler (EV_PERF_SAMPLE, perf_sample, arg);
  event_register_handler (EV_SESS_NEW, sess_created, arg);
  event_register_handler (EV_SESS_DESTROYED, sess_destroyed, arg);
  event_register_handler (EV_CALL_SEND_START, call_send_start, arg);
  event_register_handler (EV_CALL_SEND_STOP, send_stop, arg);
}

static void
dump (void)
{
  double min, avg, stddev, delta;
  int i;
  time_t start_t = (time_t)test_time_start;
  time_t stop_t = (time_t)test_time_stop;
  char start_s[20], stop_s[20];

  delta = test_time_stop - test_time_start;

  putchar ('\n');

  strftime(start_s, sizeof(start_s), "%Y-%m-%d %H:%M:%S", localtime(&start_t));
  strftime(stop_s, sizeof(stop_s), "%Y-%m-%d %H:%M:%S", localtime(&stop_t));
  printf ("Cwmp testing time: begin %s end %s\n", start_s, stop_s);

  if (0 == param.forever)
  {
    printf ("Cwmp session generation time: %.3f s\n",
            sess_time_stop - sess_time_start);
  }

  avg = 0;
  stddev = 0;
  if (delta > 0)
    avg = st.num_completed/ delta;
  if (st.num_rate_samples > 1)
    stddev = STDDEV (st.rate_sum, st.rate_sum2, st.num_rate_samples);

  if (st.num_rate_samples > 0)
    min = st.rate_min;
  else
    min = 0.0;
  
  printf ("Cwmp session rate [sess/s]: min %.2f avg %.2f max %.2f stddev %.2f\n",
	  min, avg, st.rate_max, stddev);

  avg = 0.0;
  if (st.num_succeeded > 0)
    avg = st.lifetime_sum/st.num_succeeded;
  printf ("Cwmp session lifetime [s]: %.1f\n", avg);

  avg = st.num_succeeded * 100 / (double) cwmp_num_sessions_generated;
  printf ("Cwmp session succeeded [sess]: total %d (%.1f%%)\n",
          st.num_succeeded, avg);
  printf ("Cwmp session failed [sess]: total %d (%.1f%%) work-flow %d "
          "bad-request %d no-response %d others %d\n",
          st.num_err_workflow + st.num_err_bad_req + st.num_err_others + st.num_err_no_resq,
          100 - avg, st.num_err_workflow, st.num_err_bad_req,
          st.num_err_no_resq, st.num_err_others);
  printf ("Cwmp size sent rate [B/sess]: %zu (total %zu)\n",
          st.req_bytes_sent / cwmp_num_sessions_generated, st.req_bytes_sent);
}

Stat_Collector cwmp_stat =
{
    "collects cwmp-related statistics",
    init,
    no_op,
    no_op,
    dump
};

