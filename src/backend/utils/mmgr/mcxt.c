/*-------------------------------------------------------------------------
 *
 * mcxt.c
 *	  POSTGRES memory context management code.
 *
 * This module handles context management operations that are independent
 * of the particular kind of context being operated on.  It calls
 * context-type-specific operations via the function pointers in a
 * context's MemoryContextMethods struct.
 *
 *
 * Portions Copyright (c) 2007-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/mcxt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memaccounting.h"

#include "cdb/cdbvars.h"                    /* gp_process_memory_cutoff_bytes */
#include "inttypes.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>                         /* SIZE_MAX (C99) */
#endif
#ifndef SIZE_MAX
#define SIZE_MAX ((Size)0-(Size)1)          /* for Solaris */
#endif

#ifdef CDB_PALLOC_CALLER_ID
#define CDB_MCXT_WHERE(context) (context)->callerFile, (context)->callerLine
#else
#define CDB_MCXT_WHERE(context) __FILE__, __LINE__
#endif

#if defined(CDB_PALLOC_TAGS) && !defined(CDB_PALLOC_CALLER_ID)
#error "If CDB_PALLOC_TAGS is defined, CDB_PALLOC_CALLER_ID must be defined too"
#endif

/* Maximum allowed length of the name of a context including the parent names prepended */
#define MAX_CONTEXT_NAME_SIZE 200

/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

/*
 * CurrentMemoryContext
 *		Default memory context for allocations.
 */
MemoryContext CurrentMemoryContext = NULL;

/*
 * Standard top-level contexts. For a description of the purpose of each
 * of these contexts, refer to src/backend/utils/mmgr/README
 */
MemoryContext TopMemoryContext = NULL;
MemoryContext ErrorContext = NULL;
MemoryContext PostmasterContext = NULL;
MemoryContext CacheMemoryContext = NULL;
MemoryContext MessageContext = NULL;
MemoryContext TopTransactionContext = NULL;
MemoryContext CurTransactionContext = NULL;
MemoryContext MemoryAccountMemoryContext = NULL;
MemoryContext DispatcherContext = NULL;
MemoryContext InterconnectContext = NULL;

/* This is a transient link to the active portal's memory context: */
MemoryContext PortalContext = NULL;

static void MemoryContextCallResetCallbacks(MemoryContext context);
static void MemoryContextStatsInternal(MemoryContext context, int level,
						   bool print, int max_children,
						   MemoryContextCounters *totals);

/*
 * You should not do memory allocations within a critical section, because
 * an out-of-memory error will be escalated to a PANIC. To enforce that
 * rule, the allocation functions Assert that.
 */
/*
 * GPDB_94_MERGE_FIXME: Disabled temporarily, we were unsafe things in GPDB.
 * Fix all the failures and re-enable this later.
 */
#if 0
#define AssertNotInCriticalSection(context) \
	Assert(CritSectionCount == 0 || (context)->allowInCritSection)
#else
#define AssertNotInCriticalSection(context) 
#endif

/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * MemoryContextInit
 *		Start up the memory-context subsystem.
 *
 * This must be called before creating contexts or allocating memory in
 * contexts.  TopMemoryContext and ErrorContext are initialized here;
 * other contexts must be created afterwards.
 *
 * In normal multi-backend operation, this is called once during
 * postmaster startup, and not at all by individual backend startup
 * (since the backends inherit an already-initialized context subsystem
 * by virtue of being forked off the postmaster).  But in an EXEC_BACKEND
 * build, each process must do this for itself.
 *
 * In a standalone backend this must be called during backend startup.
 */
void
MemoryContextInit(void)
{
	AssertState(TopMemoryContext == NULL);
	AssertState(CurrentMemoryContext == NULL);
	AssertState(MemoryAccountMemoryContext == NULL);

	/*
	 * Initialize TopMemoryContext as an AllocSetContext with slow growth rate
	 * --- we don't really expect much to be allocated in it.
	 *
	 * (There is special-case code in MemoryContextCreate() for this call.)
	 */
	TopMemoryContext = AllocSetContextCreate((MemoryContext) NULL,
											 "TopMemoryContext",
											 0,
											 8 * 1024,
											 8 * 1024);

	/*
	 * Not having any other place to point CurrentMemoryContext, make it point
	 * to TopMemoryContext.  Caller should change this soon!
	 */
	CurrentMemoryContext = TopMemoryContext;

	/*
	 * Initialize ErrorContext as an AllocSetContext with slow growth rate ---
	 * we don't really expect much to be allocated in it. More to the point,
	 * require it to contain at least 8K at all times. This is the only case
	 * where retained memory in a context is *essential* --- we want to be
	 * sure ErrorContext still has some memory even if we've run out
	 * elsewhere! Also, allow allocations in ErrorContext within a critical
	 * section. Otherwise a PANIC will cause an assertion failure in the error
	 * reporting code, before printing out the real cause of the failure.
	 *
	 * This should be the last step in this function, as elog.c assumes memory
	 * management works once ErrorContext is non-null.
	 */
	ErrorContext = AllocSetContextCreate(TopMemoryContext,
										 "ErrorContext",
										 8 * 1024,
										 8 * 1024,
										 8 * 1024);
	MemoryAccounting_Reset();

	MemoryContextAllowInCriticalSection(ErrorContext, true);
}

/*
 * MemoryContextReset
 *		Release all space allocated within a context and delete all its
 *		descendant contexts (but not the named context itself).
 */
void
MemoryContextReset(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	/* save a function call in common case where there are no children */
	if (context->firstchild != NULL)
		MemoryContextDeleteChildren(context);

	/* save a function call if no pallocs since startup or last reset */
	if (!context->isReset)
		MemoryContextResetOnly(context);
}

/*
 * MemoryContextResetOnly
 *		Release all space allocated within a context.
 *		Nothing is done to the context's descendant contexts.
 */
void
MemoryContextResetOnly(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	/* Nothing to do if no pallocs since startup or last reset */
	if (!context->isReset)
	{
		MemoryContextCallResetCallbacks(context);
		(*context->methods.reset) (context);
		context->isReset = true;
		VALGRIND_DESTROY_MEMPOOL(context);
		VALGRIND_CREATE_MEMPOOL(context, 0, false);
	}
}

/*
 * MemoryContextResetChildren
 *		Release all space allocated within a context's descendants,
 *		but don't delete the contexts themselves.  The named context
 *		itself is not touched.
 */
void
MemoryContextResetChildren(MemoryContext context)
{
	MemoryContext child;

	AssertArg(MemoryContextIsValid(context));

	for (child = context->firstchild; child != NULL; child = child->nextchild)
	{
		MemoryContextResetChildren(child);
		MemoryContextResetOnly(child);
	}
}

/*
 * MemoryContextDelete
 *		Delete a context and its descendants, and release all space
 *		allocated therein.
 *
 * The type-specific delete routine removes all subsidiary storage
 * for the context, but we have to delete the context node itself,
 * as well as recurse to get the children.  We must also delink the
 * node from its parent, if it has one.
 */
void
MemoryContextDeleteImpl(MemoryContext context, const char* sfile, const char *func, int sline)
{
	AssertArg(MemoryContextIsValid(context));
	/* We had better not be deleting TopMemoryContext ... */
	Assert(context != TopMemoryContext);
	/* And not CurrentMemoryContext, either */
	Assert(context != CurrentMemoryContext);

#ifdef CDB_PALLOC_CALLER_ID
	context->callerFile = sfile;
	context->callerLine = sline;
#endif

	MemoryContextDeleteChildren(context);

	/*
	 * It's not entirely clear whether 'tis better to do this before or after
	 * delinking the context; but an error in a callback will likely result in
	 * leaking the whole context (if it's not a root context) if we do it
	 * after, so let's do it before.
	 */
	MemoryContextCallResetCallbacks(context);

	/*
	 * We delink the context from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted contexts still attached
	 * to the context tree.  Better a leak than a crash.
	 */
	MemoryContextSetParent(context, NULL);

	(*context->methods.delete_context) (context);
	VALGRIND_DESTROY_MEMPOOL(context);
	pfree(context);
}

/*
 * MemoryContextDeleteChildren
 *		Delete all the descendants of the named context and release all
 *		space allocated therein.  The named context itself is not touched.
 */
void
MemoryContextDeleteChildren(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	/*
	 * MemoryContextDelete will delink the child from me, so just iterate as
	 * long as there is a child.
	 */
	while (context->firstchild != NULL)
		MemoryContextDelete(context->firstchild);
}

/*
 * MemoryContextRegisterResetCallback
 *		Register a function to be called before next context reset/delete.
 *		Such callbacks will be called in reverse order of registration.
 *
 * The caller is responsible for allocating a MemoryContextCallback struct
 * to hold the info about this callback request, and for filling in the
 * "func" and "arg" fields in the struct to show what function to call with
 * what argument.  Typically the callback struct should be allocated within
 * the specified context, since that means it will automatically be freed
 * when no longer needed.
 *
 * There is no API for deregistering a callback once registered.  If you
 * want it to not do anything anymore, adjust the state pointed to by its
 * "arg" to indicate that.
 */
void
MemoryContextRegisterResetCallback(MemoryContext context,
								   MemoryContextCallback *cb)
{
	AssertArg(MemoryContextIsValid(context));

	/* Push onto head so this will be called before older registrants. */
	cb->next = context->reset_cbs;
	context->reset_cbs = cb;
	/* Mark the context as non-reset (it probably is already). */
	context->isReset = false;
}

/*
 * MemoryContextCallResetCallbacks
 *		Internal function to call all registered callbacks for context.
 */
static void
MemoryContextCallResetCallbacks(MemoryContext context)
{
	MemoryContextCallback *cb;

	/*
	 * We pop each callback from the list before calling.  That way, if an
	 * error occurs inside the callback, we won't try to call it a second time
	 * in the likely event that we reset or delete the context later.
	 */
	while ((cb = context->reset_cbs) != NULL)
	{
		context->reset_cbs = cb->next;
		(*cb->func) (cb->arg);
	}
}

/*
 * MemoryContextSetParent
 *		Change a context to belong to a new parent (or no parent).
 *
 * We provide this as an API function because it is sometimes useful to
 * change a context's lifespan after creation.  For example, a context
 * might be created underneath a transient context, filled with data,
 * and then reparented underneath CacheMemoryContext to make it long-lived.
 * In this way no special effort is needed to get rid of the context in case
 * a failure occurs before its contents are completely set up.
 *
 * Callers often assume that this function cannot fail, so don't put any
 * elog(ERROR) calls in it.
 *
 * A possible caller error is to reparent a context under itself, creating
 * a loop in the context graph.  We assert here that context != new_parent,
 * but checking for multi-level loops seems more trouble than it's worth.
 */
void
MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
	AssertArg(MemoryContextIsValid(context));
	AssertArg(context != new_parent);

	/* Fast path if it's got correct parent already */
	if (new_parent == context->parent)
		return;

	/* Delink from existing parent, if any */
	if (context->parent)
	{
		MemoryContext parent = context->parent;

		if (context->prevchild != NULL)
			context->prevchild->nextchild = context->nextchild;
		else
		{
			Assert(parent->firstchild == context);
			parent->firstchild = context->nextchild;
		}

		if (context->nextchild != NULL)
			context->nextchild->prevchild = context->prevchild;
	}

	/* And relink */
	if (new_parent)
	{
		AssertArg(MemoryContextIsValid(new_parent));
		context->parent = new_parent;
		context->prevchild = NULL;
		context->nextchild = new_parent->firstchild;
		if (new_parent->firstchild != NULL)
			new_parent->firstchild->prevchild = context;
		new_parent->firstchild = context;
	}
	else
	{
		context->parent = NULL;
		context->prevchild = NULL;
		context->nextchild = NULL;
	}
}

/*
 * MemoryContextAllowInCriticalSection
 *		Allow/disallow allocations in this memory context within a critical
 *		section.
 *
 * Normally, memory allocations are not allowed within a critical section,
 * because a failure would lead to PANIC.  There are a few exceptions to
 * that, like allocations related to debugging code that is not supposed to
 * be enabled in production.  This function can be used to exempt specific
 * memory contexts from the assertion in palloc().
 */
void
MemoryContextAllowInCriticalSection(MemoryContext context, bool allow)
{
	AssertArg(MemoryContextIsValid(context));

	context->allowInCritSection = allow;
}

/*
 * GetMemoryChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 *
 * This is useful for measuring the total space occupied by a set of
 * allocated chunks.
 */
Size
GetMemoryChunkSpace(void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->sharedHeader->context));

	return (*header->sharedHeader->context->methods.get_chunk_space) (header->sharedHeader->context,
														 pointer);
}

/*
 * GetMemoryChunkContext
 *		Given a currently-allocated chunk, determine the context
 *		it belongs to.
 */
MemoryContext
GetMemoryChunkContext(void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->sharedHeader->context));

	return header->sharedHeader->context;
}

/*
 * MemoryContextGetParent
 *		Get the parent context (if any) of the specified context
 */
MemoryContext
MemoryContextGetParent(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	return context->parent;
}

/*
 * MemoryContextIsEmpty
 *		Is a memory context empty of any allocated space?
 */
bool
MemoryContextIsEmpty(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	/*
	 * For now, we consider a memory context nonempty if it has any children;
	 * perhaps this should be changed later.
	 */
	if (context->firstchild != NULL)
		return false;
	/* Otherwise use the type-specific inquiry */
	return (*context->methods.is_empty) (context);
}


/*
 * MemoryContextNoteAlloc
 *		Update lifetime cumulative statistics upon allocation from host mem mgr.
 *
 * Called by the context-type-specific memory manager upon successfully
 * obtaining a block of size 'nbytes' from its lower-level source (e.g. malloc).
 */
void
MemoryContextNoteAlloc(MemoryContext context, Size nbytes)
{
    Size            held;

    AssertArg(MemoryContextIsValid(context));

    for (;;)
    {
        Assert(context->allBytesAlloc >= context->allBytesFreed);
        Assert(context->allBytesAlloc - context->allBytesFreed < SIZE_MAX - nbytes);

        context->allBytesAlloc += nbytes;

        held = (Size)(context->allBytesAlloc - context->allBytesFreed);
        if (context->maxBytesHeld < held)
            context->maxBytesHeld = held;

        if (!context->parent)
            break;
        context = context->parent;
    }
}                               /* MemoryContextNoteAlloc */

/*
 * MemoryContextNoteFree
 *		Update lifetime cumulative statistics upon free to host memory manager.
 *
 * Called by the context-type-specific memory manager upon relinquishing a
 * block of size 'nbytes' back to its lower-level source (e.g. free()).
 */
void
MemoryContextNoteFree(MemoryContext context, Size nbytes)
{
    Size    held;

	AssertArg(MemoryContextIsValid(context));

    while (context)
    {
        Assert(context->allBytesAlloc >= context->allBytesFreed + nbytes);
        Assert(context->allBytesFreed + nbytes >= context->allBytesFreed);

        context->allBytesFreed += nbytes;

        held = (Size)(context->allBytesAlloc - context->allBytesFreed);
        if (context->localMinHeld > held)
            context->localMinHeld = held;

        context = context->parent;
    }
}                               /* MemoryContextNoteFree */

/*
 * MemoryContextError
 *		Report failure of a memory context operation.  Does not return.
 */
void
MemoryContextError(int errorcode, MemoryContext context,
		const char *sfile, int sline,
		const char *fmt, ...)
{
	va_list args;
	char    buf[200];

	/*
	 * Don't use elog, as we might have a malloc problem.
	 * Also, don't use write_log, as this method might be
	 * called from syslogger, which does not support
	 * write_log calls
	 */
	write_stderr("Logging memory usage for memory context error");

	MemoryAccounting_SaveToLog();
	MemoryContextStats(TopMemoryContext);

	if(coredump_on_memerror)
	{
		/*
		 * Turn memory context into a SIGSEGV, so will generate
		 * a core dump.
		 *
		 * XXX What is the right way of doing this?
		 */
		((void(*)()) NULL)();
	}

	if(errorcode != ERRCODE_OUT_OF_MEMORY && errorcode != ERRCODE_INTERNAL_ERROR)
	{
		Assert(!"Memory context error: unknown error code.");
	}

	/* Format caller's message. */
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf)-32, fmt, args);
	va_end(args);

	/*
	 * This might fail if we run out of memory at the system level
	 * (i.e., malloc returned null), and the system is running so
	 * low in memory that ereport cannot format its parameter.
	 * However, we already dumped our usage information using
	 * write_stderr, so we are gonna take a chance by calling ereport.
	 * If we fail, we at least have OOM message in the log. If we succeed,
	 * we will also have the detail error code and location of the error.
	 * Note, ereport should switch to ErrorContext which should have
	 * some preallocated memory to handle this message. Therefore,
	 * our chance of success is quite high
	 */
	ereport(ERROR, (errcode(errorcode),
				errmsg("%s (context '%s') (%s:%d)",
					buf,
					context->name,
					sfile ? sfile : "",
					sline)
		       ));

	/* not reached */
	abort();
}                               /* MemoryContextError */


/*
 * MemoryContextGetCurrentSpace
 *		Return the number of bytes currently occupied by the memory context.
 *
 * This is the amount of space obtained from the lower-level source of the
 * memory (e.g. malloc) and not yet released back to that source.  Includes
 * overhead and free space held and managed within this context by the
 * context-type-specific memory manager.
 */
Size
MemoryContextGetCurrentSpace(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));
    Assert(context->allBytesAlloc >= context->allBytesFreed);
    Assert(context->allBytesAlloc - context->allBytesFreed < SIZE_MAX);

    return (Size)(context->allBytesAlloc - context->allBytesFreed);
}                               /* MemoryContextGetCurrentSpace */

/*
 * MemoryContextGetPeakSpace
 *		Return the peak number of bytes occupied by the memory context.
 *
 * This is the maximum value reached by MemoryContextGetCurrentSpace() since
 * the context was created, or since reset by MemoryContextSetPeakSpace().
 */
Size
MemoryContextGetPeakSpace(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));
    return context->maxBytesHeld;
}                               /* MemoryContextGetPeakSpace */

/*
 * MemoryContextSetPeakSpace
 *		Resets the peak space statistic to the space currently occupied or
 *      the specified value, whichever is greater.  Returns the former peak
 *      space value.
 *
 * Can be used to observe local maximum usage over an interval and then to
 * restore the overall maximum.
 */
Size
MemoryContextSetPeakSpace(MemoryContext context, Size nbytes)
{
    Size    held;
    Size    oldpeak;

	AssertArg(MemoryContextIsValid(context));
    Assert(context->allBytesAlloc >= context->allBytesFreed);
    Assert(context->allBytesAlloc - context->allBytesFreed < SIZE_MAX);

    oldpeak = context->maxBytesHeld;

    held = (Size)(context->allBytesAlloc - context->allBytesFreed);
    context->maxBytesHeld = Max(held, nbytes);

    return oldpeak;
}                               /* MemoryContextSetPeakSpace */


/*
 * MemoryContextName
 *		Format the name of the memory context into the caller's buffer.
 *
 * Returns ptr to the name string within the supplied buffer.  (The string
 * is built at the tail of the buffer from right to left.)
 */
char *
MemoryContextName(MemoryContext context, MemoryContext relativeTo,
                  char *buf, int bufsize)
{
    MemoryContext   ctx;
    char           *cbp = buf + bufsize - 1;

	AssertArg(MemoryContextIsValid(context));

    if (bufsize <= 0)
        return buf;

    for (ctx = context; ctx && ctx != relativeTo; ctx = ctx->parent)
    {
        const char *name = ctx->name ? ctx->name : "";
        int         len = strlen(name);

        if (cbp - buf < len + 1)
        {
            len = Min(3, cbp - buf);
            cbp -= len;
            memcpy(cbp, "...", len);
            break;
        }
        if (ctx != context)
            *--cbp = '/';
        cbp -= len;
        memcpy(cbp, name, len);
    }

    if (buf < cbp)
    {
        if (!ctx)
            *--cbp = '/';
        else if (ctx == context)
            *--cbp = '.';
    }

    buf[bufsize-1] = '\0';
    return cbp;
}                               /* MemoryContextName */

/*
 * GPDB_96_MERGE_FIXME: revisit here to check if Greenplum's
 * MemoryContextStats_recur() and MemoryContext_LogContextStats() are better at
 * some places.
 */
/*
 * MemoryContextStats
 *		Print statistics about the named context and all its descendants.
 *
 * This is just a debugging utility, so it's not very fancy.  However, we do
 * make some effort to summarize when the output would otherwise be very long.
 * The statistics are sent to stderr.
 */
void
MemoryContextStats(MemoryContext context)
{
	/* A hard-wired limit on the number of children is usually good enough */
	MemoryContextStatsDetail(context, 100);
}

/*
 * MemoryContextStatsDetail
 *
 * Entry point for use if you want to vary the number of child contexts shown.
 */
void
MemoryContextStatsDetail(MemoryContext context, int max_children)
{
	MemoryContextCounters grand_totals;

	memset(&grand_totals, 0, sizeof(grand_totals));

	MemoryContextStatsInternal(context, 0, true, max_children, &grand_totals);

	fprintf(stderr,
	"Grand total: %zu bytes in %zd blocks; %zu free (%zd chunks); %zu used\n",
			grand_totals.totalspace, grand_totals.nblocks,
			grand_totals.freespace, grand_totals.freechunks,
			grand_totals.totalspace - grand_totals.freespace);
}

/*
 * MemoryContextStatsInternal
 *		One recursion level for MemoryContextStats
 *
 * Print this context if print is true, but in any case accumulate counts into
 * *totals (if given).
 */
static void
MemoryContextStatsInternal(MemoryContext context, int level,
						   bool print, int max_children,
						   MemoryContextCounters *totals)
{
	MemoryContextCounters local_totals;
	MemoryContext child;
	int			ichild;

	AssertArg(MemoryContextIsValid(context));

	/* Examine the context itself */
	(*context->methods.stats) (context, level, print, totals);

	/*
	 * Examine children.  If there are more than max_children of them, we do
	 * not print the rest explicitly, but just summarize them.
	 */
	memset(&local_totals, 0, sizeof(local_totals));

	for (child = context->firstchild, ichild = 0;
		 child != NULL;
		 child = child->nextchild, ichild++)
	{
		if (ichild < max_children)
			MemoryContextStatsInternal(child, level + 1,
									   print, max_children,
									   totals);
		else
			MemoryContextStatsInternal(child, level + 1,
									   false, max_children,
									   &local_totals);
	}

	/* Deal with excess children */
	if (ichild > max_children)
	{
		if (print)
		{
			int			i;

			for (i = 0; i <= level; i++)
				fprintf(stderr, "  ");
			fprintf(stderr,
					"%d more child contexts containing %zu total in %zd blocks; %zu free (%zd chunks); %zu used\n",
					ichild - max_children,
					local_totals.totalspace,
					local_totals.nblocks,
					local_totals.freespace,
					local_totals.freechunks,
					local_totals.totalspace - local_totals.freespace);
		}

		if (totals)
		{
			totals->nblocks += local_totals.nblocks;
			totals->freechunks += local_totals.freechunks;
			totals->totalspace += local_totals.totalspace;
			totals->freespace += local_totals.freespace;
		}
	}
}

/*
 * MemoryContextCheck
 *		Check all chunks in the named context.
 *
 * This is just a debugging utility, so it's not fancy.
 */
#ifdef MEMORY_CONTEXT_CHECKING
void
MemoryContextCheck(MemoryContext context)
{
	MemoryContext child;

	AssertArg(MemoryContextIsValid(context));

	(*context->methods.check) (context);
	for (child = context->firstchild; child != NULL; child = child->nextchild)
		MemoryContextCheck(child);
}
#endif

/*
 * MemoryContextContains
 *		Detect whether an allocated chunk of memory belongs to a given
 *		context or not.
 *
 * Note: this test assumes that the pointer was allocated using palloc.
 * If unsure, please use the generic version (MemoryContextContainsGenericAllocation).
 *
 * Caution: this test is reliable as long as the 'pointer' does point to
 * a chunk of memory allocated from *some* context.  If 'pointer' points
 * at memory obtained in some other way, there is a small chance of a
 * false-positive result since the bits right before it might look like
 * a valid chunk header by chance. In the latter case (when the memory
 * was not palloc'ed), we are more likely to crash. Please use the generic
 * version of this method if you have any doubt that the tested memory
 * region may not be palloc'ed.
 */
bool
MemoryContextContains(MemoryContext context, void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	if (pointer == NULL || pointer != (void *) MAXALIGN(pointer))
	{
		return false;
	}

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	SharedChunkHeader *sharedHeader = (SharedChunkHeader *)header->sharedHeader;

	return sharedHeader->context == context;
}

/*
 * MemoryContextContainsGenericAllocation
 *		Detects whether a generic (may or may not be allocated by
 *		palloc) chunk of memory belongs to a given context or not.
 *		Note, the "generic" means it will be ready to
 *		handle chunks not allocated using palloc.
 *
 * Caution: this test has the same problem as MemoryContextContains
 * 		where it can falsely detect a chunk belonging to a context,
 * 		while it does not. In addition, it can also falsely conclude
 * 		that a chunk does *not* belong to a context, while in reality
 * 		it does. The latter weakness stems from its versatility to
 * 		handle non-palloc'ed chunks.
 */
bool
MemoryContextContainsGenericAllocation(MemoryContext context, void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	if (pointer == NULL || pointer != (void *) MAXALIGN(pointer))
	{
		return false;
	}

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AllocSet set = (AllocSet)context;

	if (header->sharedHeader == set->sharedHeaderList ||
			(set->sharedHeaderList != NULL && set->sharedHeaderList->next == header->sharedHeader) ||
			(set->sharedHeaderList != NULL && set->sharedHeaderList->next != NULL && set->sharedHeaderList->next->next == header->sharedHeader))
	{
		/*
		 * At this point we know that one of the sharedHeader pointers of the
		 * provided context (AllocSet) is the same as the sharedHeader
		 * pointer of the provided chunk. Therefore, the chunk should
		 * belong to the AllocSet (with a false positive chance coming
		 * from some third party allocated memory region having the
		 * same value as the sharedHeaderList pointer address
		 */
		return true;
	}

	/*
	 * We might falsely conclude that the chunk does not belong
	 * to the context, if we fail to match the chunk's sharedHeader
	 * pointer with one of the leading sharedHeader pointers in the
	 * context's sharedHeaderList.
	 */
	return false;
}

/*--------------------
 * MemoryContextCreate
 *		Context-type-independent part of context creation.
 *
 * This is only intended to be called by context-type-specific
 * context creation routines, not by the unwashed masses.
 *
 * The context creation procedure is a little bit tricky because
 * we want to be sure that we don't leave the context tree invalid
 * in case of failure (such as insufficient memory to allocate the
 * context node itself).  The procedure goes like this:
 *	1.  Context-type-specific routine first calls MemoryContextCreate(),
 *		passing the appropriate tag/size/methods values (the methods
 *		pointer will ordinarily point to statically allocated data).
 *		The parent and name parameters usually come from the caller.
 *	2.  MemoryContextCreate() attempts to allocate the context node,
 *		plus space for the name.  If this fails we can ereport() with no
 *		damage done.
 *	3.  We fill in all of the type-independent MemoryContext fields.
 *	4.  We call the type-specific init routine (using the methods pointer).
 *		The init routine is required to make the node minimally valid
 *		with zero chance of failure --- it can't allocate more memory,
 *		for example.
 *	5.  Now we have a minimally valid node that can behave correctly
 *		when told to reset or delete itself.  We link the node to its
 *		parent (if any), making the node part of the context tree.
 *	6.  We return to the context-type-specific routine, which finishes
 *		up type-specific initialization.  This routine can now do things
 *		that might fail (like allocate more memory), so long as it's
 *		sure the node is left in a state that delete will handle.
 *
 * This protocol doesn't prevent us from leaking memory if step 6 fails
 * during creation of a top-level context, since there's no parent link
 * in that case.  However, if you run out of memory while you're building
 * a top-level context, you might as well go home anyway...
 *
 * Normally, the context node and the name are allocated from
 * TopMemoryContext (NOT from the parent context, since the node must
 * survive resets of its parent context!).  However, this routine is itself
 * used to create TopMemoryContext!  If we see that TopMemoryContext is NULL,
 * we assume we are creating TopMemoryContext and use malloc() to allocate
 * the node.
 *
 * Note that the name field of a MemoryContext does not point to
 * separately-allocated storage, so it should not be freed at context
 * deletion.
 *--------------------
 */
MemoryContext
MemoryContextCreate(NodeTag tag, Size size,
					MemoryContextMethods *methods,
					MemoryContext parent,
					const char *name)
{
	MemoryContext node;
	Size		needed = size + strlen(name) + 1;

	// GPDB_94_MERGE_FIXME: same as AssertNotInCriticalSection
#if 0
	/* creating new memory contexts is not allowed in a critical section */
	Assert(CritSectionCount == 0);
#endif

	/* Get space for node and name */
	if (TopMemoryContext != NULL)
	{
		/* Normal case: allocate the node in TopMemoryContext */
		node = (MemoryContext) MemoryContextAlloc(TopMemoryContext,
												  needed);
	}
	else
	{
		/* Special case for startup: use good ol' malloc */
		node = (MemoryContext) malloc(needed);
		if(!node)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("Failed to create memory context: out of memory")
				));
	}

	/* Initialize the node as best we can */
	MemSet(node, 0, size);
	node->type = tag;
	node->methods = *methods;
	node->parent = parent;
	node->firstchild = NULL;
	node->prevchild = NULL;
	node->nextchild = NULL;
	node->isReset = true;
	node->name = ((char *) node) + size;
	strcpy(node->name, name);

	/* Type-specific routine finishes any other essential initialization */
	(*node->methods.init) (node);

	/* OK to link node to parent (if any) */
	/* Could use MemoryContextSetParent here, but doesn't seem worthwhile */
	if (parent)
	{
		node->nextchild = parent->firstchild;
		if (parent->firstchild != NULL)
			parent->firstchild->prevchild = node;
		parent->firstchild = node;
		/* inherit allowInCritSection flag from parent */
		node->allowInCritSection = parent->allowInCritSection;
	}

	VALGRIND_CREATE_MEMPOOL(node, 0, false);

	/* Return to type-specific creation routine to finish up */
	return node;
}

/*
 * MemoryContextAlloc
 *		Allocate space within the specified context.
 *
 * This could be turned into a macro, but we'd have to import
 * nodes/memnodes.h into postgres.h which seems a bad idea.
 */
void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	void	   *ret;
#ifdef PGTRACE_ENABLED
	StandardChunkHeader *header;
#endif

	AssertArg(MemoryContextIsValid(context));

#ifdef CDB_PALLOC_CALLER_ID
	context->callerFile = sfile;
	context->callerLine = sline;
#endif

	if (!AllocSizeIsValid(size))
		MemoryContextError(ERRCODE_INTERNAL_ERROR,
				context, CDB_MCXT_WHERE(context),
				"invalid memory alloc request size %lu",
				(unsigned long)size);

	context->isReset = false;

	ret = (*context->methods.alloc) (context, size);
	if (ret == NULL)
	{
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

#ifdef PGTRACE_ENABLED
	header = (StandardChunkHeader *)
		((char *) ret - STANDARDCHUNKHEADERSIZE);
	PG_TRACE5(memctxt__alloc, size, header->size, 0, 0, (long) context->name);
#endif

	return ret;
}

/*
 * MemoryContextAllocZero
 *		Like MemoryContextAlloc, but clears allocated memory
 *
 *	We could just call MemoryContextAlloc then clear the memory, but this
 *	is a very common combination, so we provide the combined operation.
 */
void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	void	   *ret;

#ifdef PGTRACE_ENABLED
	StandardChunkHeader *header;
#endif
	AssertArg(MemoryContextIsValid(context));

#ifdef CDB_PALLOC_CALLER_ID
	context->callerFile = sfile;
	context->callerLine = sline;
#endif

	if (!AllocSizeIsValid(size))
		MemoryContextError(ERRCODE_INTERNAL_ERROR,
				context, CDB_MCXT_WHERE(context),
				"invalid memory alloc request size %lu",
				(unsigned long)size);

	context->isReset = false;

	ret = (*context->methods.alloc) (context, size);
	if (ret == NULL)
	{
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

#ifdef PGTRACE_ENABLED
	header = (StandardChunkHeader *)
		((char *) ret - STANDARDCHUNKHEADERSIZE);
	PG_TRACE5(memctxt__alloc, size, header->size, 0, 0, (long) context->name);
#endif

	return ret;
}

/*
 * MemoryContextAllocZeroAligned
 *		MemoryContextAllocZero where length is suitable for MemSetLoop
 *
 *	This might seem overly specialized, but it's not because newNode()
 *	is so often called with compile-time-constant sizes.
 */
void *
MemoryContextAllocZeroAligned(MemoryContext context, Size size)
{
	void	   *ret;

#ifdef PGTRACE_ENABLED
	StandardChunkHeader *header;
#endif

	AssertArg(MemoryContextIsValid(context));

#ifdef CDB_PALLOC_CALLER_ID
	context->callerFile = sfile;
	context->callerLine = sline;
#endif

	if (!AllocSizeIsValid(size))
		MemoryContextError(ERRCODE_INTERNAL_ERROR,
				context, CDB_MCXT_WHERE(context),
				"invalid memory alloc request size %lu",
				(unsigned long)size);

	context->isReset = false;

	ret = (*context->methods.alloc) (context, size);
	if (ret == NULL)
	{
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetLoop(ret, 0, size);

#ifdef PGTRACE_ENABLED
	header = (StandardChunkHeader *)
		((char *) ret - STANDARDCHUNKHEADERSIZE);
	PG_TRACE5(memctxt__alloc, size, header->size, 0, 0, (long) context->name);
#endif

	return ret;
}

/*
 * MemoryContextAllocExtended
 *		Allocate space within the specified context using the given flags.
 */
void *
MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
	void	   *ret;

	AssertArg(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (((flags & MCXT_ALLOC_HUGE) != 0 && !AllocHugeSizeIsValid(size)) ||
		((flags & MCXT_ALLOC_HUGE) == 0 && !AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = (*context->methods.alloc) (context, size);
	if (ret == NULL)
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			MemoryContextError(ERRCODE_OUT_OF_MEMORY,
							   context, CDB_MCXT_WHERE(context),
							   "Out of memory.  Failed on request of size %zu bytes.",
							   size);
		}
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc(Size size)
{
	/* duplicates MemoryContextAlloc to avoid increased overhead */
	void	   *ret;

	AssertArg(MemoryContextIsValid(CurrentMemoryContext));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	CurrentMemoryContext->isReset = false;

	ret = (*CurrentMemoryContext->methods.alloc) (CurrentMemoryContext, size);
	if (ret == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu.", size)));
	}

	VALGRIND_MEMPOOL_ALLOC(CurrentMemoryContext, ret, size);

	return ret;
}

void *
palloc0(Size size)
{
	/* duplicates MemoryContextAllocZero to avoid increased overhead */
	void	   *ret;

	AssertArg(MemoryContextIsValid(CurrentMemoryContext));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	CurrentMemoryContext->isReset = false;

	ret = (*CurrentMemoryContext->methods.alloc) (CurrentMemoryContext, size);
	if (ret == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu.", size)));
	}

	VALGRIND_MEMPOOL_ALLOC(CurrentMemoryContext, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc_extended(Size size, int flags)
{
	/* duplicates MemoryContextAllocExtended to avoid increased overhead */
	void	   *ret;

	AssertArg(MemoryContextIsValid(CurrentMemoryContext));
	AssertNotInCriticalSection(CurrentMemoryContext);

	if (((flags & MCXT_ALLOC_HUGE) != 0 && !AllocHugeSizeIsValid(size)) ||
		((flags & MCXT_ALLOC_HUGE) == 0 && !AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	CurrentMemoryContext->isReset = false;

	ret = (*CurrentMemoryContext->methods.alloc) (CurrentMemoryContext, size);
	if (ret == NULL)
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %zu.", size)));
		}
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(CurrentMemoryContext, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * pfree
 *		Release an allocated chunk.
 */
void
pfree(void *pointer)
{
	MemoryContext context;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	StandardChunkHeader* header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);
	context = header->sharedHeader->context;

	AssertArg(MemoryContextIsValid(context));

#ifdef PGTRACE_ENABLED
	PG_TRACE5(memctxt__free, 0, 0, 
#ifdef MEMORY_CONTEXT_CHECKING
		header->requested_size, header->size,
#else
		0, header->size, 
#endif
		(long) header->sharedHeader->context->name);
#endif

#ifdef CDB_PALLOC_CALLER_ID
	header->sharedHeader->context->callerFile = sfile;
	header->sharedHeader->context->callerLine = sline;
#endif

	if (context->methods.free_p)
		(*context->methods.free_p) (context, pointer);
	else
		Assert(header);   /* this assert never fails. Just here so we can set breakpoint in debugger. */
	VALGRIND_MEMPOOL_FREE(context, pointer);
}

/*
 * repalloc
 *		Adjust the size of a previously allocated chunk.
 */
void *
repalloc(void *pointer, Size size)
{
	StandardChunkHeader *header;
	MemoryContext context;
	void	   *ret;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);
	context = header->sharedHeader->context;
	AssertArg(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		MemoryContextError(ERRCODE_INTERNAL_ERROR,
				context, CDB_MCXT_WHERE(context),
				"invalid memory alloc request size %zu", size);
	/* isReset must be false already */
	Assert(!context->isReset);

#ifdef PGTRACE_ENABLED
	{
		long old_reqsize;
		long old_size;
#ifdef MEMORY_CONTEXT_CHECKING
		old_reqsize = header->requested_size;
#else
		old_reqsize = 0;
#endif
		old_size = header->size;
	}
#endif

#ifdef CDB_PALLOC_CALLER_ID
	context->callerFile = sfile;
	context->callerLine = sline;
#endif

	ret = (*context->methods.realloc) (context, pointer, size);
	if (ret == NULL)
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

#ifdef PGTRACE_ENABLED
	header = (StandardChunkHeader *)
		((char *) ret - STANDARDCHUNKHEADERSIZE);
	PG_TRACE5(memctxt__realloc, size, header->size, old_reqsize, old_size, (long) context->name);
#endif

	return ret;
}

/*
 * MemoryContextAllocHuge
 *		Allocate (possibly-expansive) space within the specified context.
 *
 * See considerations in comment at MaxAllocHugeSize.
 */
void *
MemoryContextAllocHuge(MemoryContext context, Size size)
{
	void	   *ret;

	AssertArg(MemoryContextIsValid(context));

	if (!AllocHugeSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = (*context->methods.alloc) (context, size);
	if (ret == NULL)
	{
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * repalloc_huge
 *		Adjust the size of a previously allocated chunk, permitting a large
 *		value.  The previous allocation need not have been "huge".
 */
void *
repalloc_huge(void *pointer, Size size)
{
	StandardChunkHeader *header;
	MemoryContext context;
	void	   *ret;

	if (!AllocHugeSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);
	context = header->sharedHeader->context;

	AssertArg(MemoryContextIsValid(context));

	/* isReset must be false already */
	Assert(!context->isReset);

	ret = (*context->methods.realloc) (context, pointer, size);
	if (ret == NULL)
		MemoryContextError(ERRCODE_OUT_OF_MEMORY,
						   context, CDB_MCXT_WHERE(context),
						   "Out of memory.  Failed on request of size %zu bytes.",
						   size);

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

	return ret;
}

/*
 * MemoryContextStrdup
 *		Like strdup(), but allocate from the specified context
 */
char *
MemoryContextStrdup(MemoryContext context, const char *string)
{
	char	   *nstr;
	Size		len = strlen(string) + 1;

	nstr = (char *) MemoryContextAlloc(context, len);

	memcpy(nstr, string, len);

	return nstr;
}

char *
pstrdup(const char *in)
{
	return MemoryContextStrdup(CurrentMemoryContext, in);
}

/*
 * pnstrdup
 *		Like pstrdup(), but append null byte to a
 *		not-necessarily-null-terminated input string.
 */
char *
pnstrdup(const char *in, Size len)
{
	char	   *out = palloc(len + 1);

	memcpy(out, in, len);
	out[len] = '\0';
	return out;
}

#if defined(WIN32) || defined(__CYGWIN__)
/*
 *	Memory support routines for libpgport on Win32
 *
 *	Win32 can't load a library that PGDLLIMPORTs a variable
 *	if the link object files also PGDLLIMPORT the same variable.
 *	For this reason, libpgport can't reference CurrentMemoryContext
 *	in the palloc macro calls.
 *
 *	To fix this, we create several functions here that allow us to
 *	manage memory without doing the inline in libpgport.
 */
void *
pgport_palloc(Size sz)
{
	return palloc(sz);
}


char *
pgport_pstrdup(const char *str)
{
	return pstrdup(str);
}


/* Doesn't reference a PGDLLIMPORT variable, but here for completeness. */
void
pgport_pfree(void *pointer)
{
	pfree(pointer);
}

#endif
