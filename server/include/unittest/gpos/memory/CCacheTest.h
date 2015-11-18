//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC CORP.
//
//	@filename:
//		CCacheTest.h
//
//	@doc:
//		Test for CCache
//
//	@owner:
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef GPOS_CCACHETEST_H_
#define GPOS_CCACHETEST_H_

#include "gpos/memory/CCache.h"
#include "gpos/common/CList.h"

#include "gpos/memory/CCache.h"
#include "gpos/memory/CCacheAccessor.h"


namespace gpos
{
	//---------------------------------------------------------------------------
	//	@class:
	//		CCacheTest
	//
	//	@doc:
	//		Static unit tests
	//
	//---------------------------------------------------------------------------
	class CCacheTest
	{

		private:

			// helper functions

			// insert elements with duplicate keys
			static GPOS_RESULT EresInsertDuplicates(CCache *);

			// remove
			static GPOS_RESULT EresRemoveDuplicates(CCache *);

			// insert/delete/lookup tasks
			static void* PvInsertTask(void *);

			static void* PvDeleteTask(void *);

			static void* PvLookupTask(void *);

			// inserts one SSimpleObject with key and value set to ulKey
			static void InsertOneElement(CCache *pCache, ULONG ulKey);

			// inserts as many SSimpleObjects as needed (starting with the key ulKeyStart and
			// sequentially generating the successive keys) to consume cache quota.
			static ULONG ULFillCacheWithoutEviction(CCache *pCache, ULONG ulKeyStart, ULLONG& ullOneElemSize, ULLONG ullCacheCapacity);


			// A simple object (no deep structures)
			struct SSimpleObject
			{
				ULONG m_ulKey;

				ULONG m_ulValue;

				SSimpleObject
					(
					ULONG ulKey,
					ULONG ulVal
					)
					:
					m_ulKey(ulKey),
					m_ulValue(ulVal)
				{

				}

				static ULONG UlMyHash
					(
					const VOID_PTR & pvKey
					)
				{
					ULONG *pul = static_cast<ULONG *> (pvKey);
					return *pul;
				}

				//key equality function
				static BOOL FMyEqual
					(
					const VOID_PTR &pvKey,
					const VOID_PTR &pvKeySecond
					);

				// equality for object-based comparison
				BOOL operator ==
					(
					const SSimpleObject &obj
					)
					const
				{
					return obj.m_ulKey == m_ulKey;
				}
			}; // struct SSimpleObject


			// An object with a deep structure
			class CDeepObject
			{

				private:

					// linked list entry
					struct SDeepObjectEntry
					{
						// a link to connect entries together
						SLink m_link;

						// entry's key
						ULONG m_ulKey;

						// entry's value
						ULONG m_ulValue;

						// ctor
						SDeepObjectEntry
							(
							ULONG ulKey,
							ULONG ulVal
							)
							:
							m_ulKey(ulKey),
							m_ulValue(ulVal)
						{}

					}; // struct SDeepObjectEntry

					// a deep structure given by a linked list
					CList<SDeepObjectEntry> m_list;

				public:

					typedef CList<SDeepObjectEntry> CDeepObjectList;

					// ctor
					CDeepObject()
					{
						m_list.Init(GPOS_OFFSET(SDeepObjectEntry, m_link));
					}

					// hashing  function
					static ULONG UlMyHash(const VOID_PTR &pvKey);

					// key equality function
					static BOOL FMyEqual
						(
						const VOID_PTR &pvKey,
						const VOID_PTR &pvKeySecond
						);

					// key accessor
					CDeepObjectList *PKey()
					{
						return &m_list;
					}

					// add a new entry to the linked list
					void AddEntry
						(
						IMemoryPool *pmp,
						ULONG ulKey,
						ULONG ulVal
						);

				}; // class CDeepObject


			// accessors type definitions
			typedef CCacheAccessor<SSimpleObject, ULONG>
				CSimpleObjectCacheAccessor;
			typedef CCacheAccessor
						<CDeepObject,
						CDeepObject::CDeepObjectList> CDeepObjectCacheAccessor;

			// cache task function pointer
			typedef void * (*TaskFuncPtr)(void *);

		public:

			// unittests
			static GPOS_RESULT EresUnittest();
			static GPOS_RESULT EresUnittest_Basic();
			static GPOS_RESULT EresUnittest_Eviction();
			static GPOS_RESULT EresUnittest_DeepObject();
			static GPOS_RESULT EresUnittest_Iteration();
			static GPOS_RESULT EresUnittest_IterativeDeletion();
			static GPOS_RESULT EresUnittest_ConcurrentAccess();


	}; // class CCacheTest

}

#endif // GPOS_CCACHETEST_H_

// EOF
