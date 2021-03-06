/*
 * Copyright (c) 2010-2011 Broadcom. All rights reserved.
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

/*=============================================================================
VideoCore OS Abstraction Layer - pthreads types
=============================================================================*/

#define  VCOS_INLINE_BODIES

#include "interface/vcos/vcos.h"
#ifdef HAVE_VCOS_VERSION
#include "interface/vcos/vcos_build_info.h"
#endif

#include <sys/kthread.h>

MALLOC_DEFINE(M_VCOS, "vcos", "VideoCore general purpose memory");

VCOS_CFG_ENTRY_T  vcos_cfg_dir;
VCOS_CFG_ENTRY_T  vcos_logging_cfg_dir;
VCOS_CFG_ENTRY_T  vcos_version_cfg;

#ifndef VCOS_DEFAULT_STACK_SIZE
#define VCOS_DEFAULT_STACK_SIZE 4096
#endif

static VCOS_THREAD_ATTR_T default_attrs = {
   0,
   VCOS_DEFAULT_STACK_SIZE,
};

/* XXXBSD: destroy on unload? */
static struct mtx vcos_mtx;
MTX_SYSINIT(vcos_mtx, &vcos_mtx, "vcos", MTX_DEF);

typedef void (*LEGACY_ENTRY_FN_T)(int, void *);

/** Wrapper function around the real thread function. Posts the semaphore
  * when completed.
  */
static void 
vcos_thread_wrapper(void *arg)
{
   void *ret;
   VCOS_THREAD_T *thread = arg;

   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->thread.proc = curproc;

   vcos_add_thread(thread);

#ifdef VCOS_WANT_TLS_EMULATION
   vcos_tls_thread_register(&thread->_tls);
#endif

   if (thread->legacy)
   {
      LEGACY_ENTRY_FN_T fn = (LEGACY_ENTRY_FN_T)thread->entry;
      fn(0,thread->arg);
      ret = 0;
   }
   else
   {
      ret = thread->entry(thread->arg);
   }

   thread->exit_data = ret;

   vcos_remove_thread(curthread);

   /* For join and cleanup */
   vcos_semaphore_post(&thread->wait);
}

VCOS_STATUS_T vcos_thread_create(VCOS_THREAD_T *thread,
                                 const char *name,
                                 VCOS_THREAD_ATTR_T *attrs,
                                 VCOS_THREAD_ENTRY_FN_T entry,
                                 void *arg)
{
   VCOS_STATUS_T st;
   struct proc *newp;

   memset(thread, 0, sizeof(*thread));
   thread->magic     = VCOS_THREAD_MAGIC;
   strlcpy( thread->name, name, sizeof( thread->name ));
   thread->legacy    = attrs ? attrs->legacy : 0;
   thread->entry = entry;
   thread->arg = arg;

   if (!name)
   {
      vcos_assert(0);
      return VCOS_EINVAL;
   }

   st = vcos_semaphore_create(&thread->wait, NULL, 0);
   if (st != VCOS_SUCCESS)
   {
      return st;
   }

   st = vcos_semaphore_create(&thread->suspend, NULL, 0);
   if (st != VCOS_SUCCESS)
   {
      return st;
   }

   /*required for event groups */
   vcos_timer_create(&thread->_timer.timer, thread->name, NULL, NULL);

   newp = NULL;
   if (kproc_create(&vcos_thread_wrapper, (void*)thread, &newp, 0, 0, "%s", name) != 0)
      panic("kproc_create failed");
   thread->thread.proc = newp;
   return VCOS_SUCCESS;
}

void vcos_thread_join(VCOS_THREAD_T *thread,
                             void **pData)
{
   vcos_assert(thread);
   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->joined = 1;

   vcos_semaphore_wait(&thread->wait);

   if (pData)
   {
      *pData = thread->exit_data;
   }

   /* Clean up */
   if (thread->stack)
      vcos_free(thread->stack);

   vcos_semaphore_delete(&thread->wait);
   vcos_semaphore_delete(&thread->suspend);

}

uint32_t vcos_getmicrosecs( void )
{
   struct timeval tv;
   getmicrouptime(&tv);
   return (tv.tv_sec*1000000) + tv.tv_usec;
}

VCOS_STATUS_T vcos_timer_init(void)
{
    return VCOS_SUCCESS;
}

static const char *log_prefix[] =
{
   "",            /* VCOS_LOG_UNINITIALIZED */
   "",            /* VCOS_LOG_NEVER */
   "[E] ",        /* VCOS_LOG_ERROR */
   "[W] ",        /* VCOS_LOG_WARN */
   "[I] ",        /* VCOS_LOG_INFO */
   "[I] "         /* VCOS_LOG_TRACE */
};

void vcos_vlog_default_impl(const VCOS_LOG_CAT_T *cat, VCOS_LOG_LEVEL_T _level, const char *fmt, va_list args)
{
   char *newline = strchr( fmt, '\n' );
   const char  *prefix;
   const char  *real_fmt;

   /* XXXBSD: preempt_disable(); */
   {
       if ( *fmt == '<' )
       {
           prefix = fmt;
           real_fmt= &fmt[3];
       }
       else
       {
          prefix = log_prefix[_level];
          real_fmt = fmt;
       }
#if defined( CONFIG_BCM_KNLLOG_SUPPORT )
       knllog_ventry( "vcos", real_fmt, args );
#endif
       printf( "%.3svcos: [%d]: ", prefix, curthread->td_proc->p_pid );
       vprintf( real_fmt, args );

       if ( newline == NULL )
       {
          printf("\n");
       }
   }
   /* XXXBSD: preempt_enable(); */
}


const char * _vcos_log_level(void)
{
   return NULL;
}

/*****************************************************************************
*
*    Displays the version information in /proc/vcos/version
*
*****************************************************************************/

#ifdef HAVE_VCOS_VERSION

static void show_version( VCOS_CFG_BUF_T buf, void *data )
{
   static const char* copyright = "Copyright (c) 2011 Broadcom";

   vcos_cfg_buf_printf( buf, "Built %s %s on %s\n%s\nversion %s\n",
                        vcos_get_build_date(),
                        vcos_get_build_time(),
                        vcos_get_build_hostname(),
                        copyright,
                        vcos_get_build_version() );
}

#endif

/*****************************************************************************
*
*    Initialises vcos
*
*****************************************************************************/

VCOS_STATUS_T vcos_init(void)
{
   if ( vcos_cfg_mkdir( &vcos_cfg_dir, NULL, "vcos" ) != VCOS_SUCCESS )
   {
      printf( "%s: Unable to create vcos cfg entry\n", __func__ );
   }
   vcos_logging_init();

#ifdef HAVE_VCOS_VERSION
   if ( vcos_cfg_create_entry( &vcos_version_cfg, &vcos_cfg_dir, "version",
                               show_version, NULL, NULL ) != VCOS_SUCCESS )
   {
      printf( "%s: Unable to create vcos cfg entry 'version'\n", __func__ );
   }
#endif

   return VCOS_SUCCESS;
}

/*****************************************************************************
*
*    Deinitializes vcos
*
*****************************************************************************/

void vcos_deinit(void)
{
#ifdef HAVE_VCOS_VERSION
   vcos_cfg_remove_entry( &vcos_version_cfg );
#endif
   vcos_cfg_remove_entry( &vcos_cfg_dir );
}

void vcos_global_lock(void)
{
   mtx_lock(&vcos_mtx);
}

void vcos_global_unlock(void)
{
   mtx_unlock(&vcos_mtx);
}

/* vcos_thread_exit() doesn't really stop this thread here
 *
 * At the moment, call to do_exit() will leak task_struct for
 * current thread, so we let the vcos_thread_wrapper() do the
 * cleanup and exit job, and we return w/o actually stopping the thread.
 *
 * ToDo: Kernel v2.6.31 onwards, it is considered safe to call do_exit()
 * from kthread, the implementation of which is combined in 2 patches
 * with commit-ids "63706172" and "cdd140bd" in oss Linux kernel tree
 */

void vcos_thread_exit(void *arg)
{
   VCOS_THREAD_T *thread = vcos_thread_current();

   vcos_assert(thread);
   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->exit_data = arg;
}

void vcos_thread_attr_init(VCOS_THREAD_ATTR_T *attrs)
{
   *attrs = default_attrs;
}

void _vcos_task_timer_set(void (*pfn)(void *), void *cxt, VCOS_UNSIGNED ms)
{
   VCOS_THREAD_T *self = vcos_thread_current();
   vcos_assert(self);
   vcos_assert(self->_timer.pfn == NULL);

   vcos_timer_create( &self->_timer.timer, "TaskTimer", pfn, cxt );
   vcos_timer_set(&self->_timer.timer, ms);
}

void _vcos_task_timer_cancel(void)
{
   VCOS_THREAD_T *self = vcos_thread_current();
   vcos_timer_cancel(&self->_timer.timer);
   vcos_timer_delete(&self->_timer.timer);
}

int vcos_vsnprintf( char *buf, size_t buflen, const char *fmt, va_list ap )
{
   return vsnprintf( buf, buflen, fmt, ap );
}

int vcos_snprintf(char *buf, size_t buflen, const char *fmt, ...)
{
   int ret;
   va_list ap;
   va_start(ap,fmt);
   ret = vsnprintf(buf, buflen, fmt, ap);
   va_end(ap);
   return ret;
}

int vcos_llthread_running(VCOS_LLTHREAD_T *t) {
   vcos_assert(0);   /* this function only exists as a nasty hack for the video codecs! */
   return 1;
}

static int vcos_verify_bkpts = 1;

int vcos_verify_bkpts_enabled(void)
{
   return vcos_verify_bkpts;
}

/*****************************************************************************
*
*    _vcos_log_platform_init is called from vcos_logging_init
*
*****************************************************************************/

void _vcos_log_platform_init(void)
{
   if ( vcos_cfg_mkdir( &vcos_logging_cfg_dir, &vcos_cfg_dir, "logging" ) != VCOS_SUCCESS )
   {
      printf( "%s: Unable to create logging cfg entry\n", __func__ );
   }
}

/*****************************************************************************
*
*    Called to display the contents of a logging category.
*
*****************************************************************************/

static void logging_show_category( VCOS_CFG_BUF_T buf, void *data )
{
   VCOS_LOG_CAT_T *category = data;

   vcos_cfg_buf_printf( buf, "%s\n", vcos_log_level_to_string( category->level ));
}

/*****************************************************************************
*
*    Called to parse content for a logging category.
*
*****************************************************************************/

static void logging_parse_category( VCOS_CFG_BUF_T buf, void *data )
{
   VCOS_LOG_CAT_T *category = data;
   const char *str = vcos_cfg_buf_get_str( buf );
   VCOS_LOG_LEVEL_T  level;

   if ( vcos_string_to_log_level( str, &level ) == VCOS_SUCCESS )
   {
      category->level = level;
   }
   else
   {
      printf( "%s: Unrecognized logging level: '%s'\n",
              __func__, str );
   }
}

/*****************************************************************************
*
*    _vcos_log_platform_register is called from vcos_log_register whenever
*    a new category is registered.
*
*****************************************************************************/

void _vcos_log_platform_register(VCOS_LOG_CAT_T *category)
{
   VCOS_CFG_ENTRY_T  entry;

   if ( vcos_cfg_create_entry( &entry, &vcos_logging_cfg_dir, category->name,
                               logging_show_category, logging_parse_category,
                               category ) != VCOS_SUCCESS )
   {
      printf( "%s: Unable to create cfg entry for logging category '%s'\n",
              __func__, category->name );
      category->platform_data = NULL;
   }
   else
   {
      category->platform_data = entry;
   }
}

/*****************************************************************************
*
*    _vcos_log_platform_unregister is called from vcos_log_unregister whenever
*    a new category is unregistered.
*
*****************************************************************************/

void _vcos_log_platform_unregister(VCOS_LOG_CAT_T *category)
{
   VCOS_CFG_ENTRY_T  entry;

   entry = category->platform_data;
   if ( entry != NULL )
   {
      if ( vcos_cfg_remove_entry( &entry ) != VCOS_SUCCESS )
      {
         printf( "%s: Unable to remove cfg entry for logging category '%s'\n",
                 __func__, category->name );
      }
   }
}

/*****************************************************************************
*
*    Allocate memory.
*
*****************************************************************************/

void *vcos_platform_malloc( VCOS_UNSIGNED required_size )
{
   if ( required_size >= ( 2 * PAGE_SIZE ))
   {
      /* For larger allocations, use vmalloc, whose underlying allocator
       * returns pages
       */

     /* XXXBSD: optimize it? */
   }

   /* For smaller allocation, use kmalloc */

   return malloc( required_size, M_VCOS, M_WAITOK | M_ZERO );
}

/*****************************************************************************
*
*    Free previously allocated memory
*
*****************************************************************************/

void  vcos_platform_free( void *ptr )
{
   free( ptr, M_VCOS );
}

/*****************************************************************************
*
*    Execute a routine exactly once.
*
*****************************************************************************/

VCOS_STATUS_T vcos_once(VCOS_ONCE_T *once_control,
                        void (*init_routine)(void))
{
   /* In order to be thread-safe we need to re-test *once_control
    * inside the lock. The outer test is basically an optimization
    * so that once it is initialized we don't need to waste time
    * trying to acquire the lock.
    */

   if ( *once_control == 0 )
   {
       vcos_global_lock();
       if ( *once_control == 0 )
       {
           init_routine();
           *once_control = 1;
       }
       vcos_global_unlock();
   }

   return VCOS_SUCCESS;
}

/*****************************************************************************
*
*    String duplication routine.
*
*****************************************************************************/

char *vcos_strdup(const char *str)
{
    return strdup(str, M_VCOS);
}
