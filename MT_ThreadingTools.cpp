// Massgate
// Copyright (C) 2017 Ubisoft Entertainment
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
#include "stdafx.h"
#include "MT_ThreadingTools.h"
#include "MT_Mutex.h"
#include "MC_Profiler.h"
#include "MT_ThreadingTools.h"

#include "MT_Thread.h"
#include "MC_debug.h"
#include "MC_StackWalker.h"

void MC_Sleep(int aMillis)
{
	if(aMillis < 0)
		aMillis = 0;

	MC_THREADPROFILER_ENTER_WAIT();

	::Sleep(aMillis);

	MC_THREADPROFILER_LEAVE_WAIT();
}

void MC_Yield()
{
	MC_THREADPROFILER_ENTER_WAIT();

	::SwitchToThread();
	//::Sleep(0);

	MC_THREADPROFILER_LEAVE_WAIT();
}

#if IS_PC_BUILD
DWORD locCountSetBits(ULONG_PTR aBitMask)
{
    DWORD lShift = sizeof(ULONG_PTR) * 8 - 1;
    DWORD bitSetCount = 0;
    ULONG_PTR bitTest = (ULONG_PTR)1 << lShift;
    DWORD i;

    for (i = 0; i <= lShift; ++i)
    {
        bitSetCount += ((aBitMask & bitTest) ? 1 : 0);
        bitTest /= 2;
    }

    return bitSetCount;
}
#endif

unsigned int MT_ThreadingTools::GetLogicalProcessorCount()
{
#if IS_PC_BUILD	// PC specific
    DWORD length = 0;
    bool done = false;
    while (!done)
    {
        DWORD ret = GetLogicalProcessorInformation(nullptr, &length);

        u32 numProcessorInfo = length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1;
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buffer = new SYSTEM_LOGICAL_PROCESSOR_INFORMATION[numProcessorInfo];
        memset(buffer, 0, numProcessorInfo * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        ret = GetLogicalProcessorInformation(buffer, &length);
        if (!ret)
        {
            delete[] buffer;
            return 1;
        }
        
        u32 numLogicalProcessors = 0;
        for (u32 infoIdx = 0; infoIdx < numProcessorInfo; ++infoIdx)
        {
            if(buffer[infoIdx].Relationship == RelationProcessorCore)
                numLogicalProcessors += locCountSetBits(buffer[infoIdx].ProcessorMask);
        }
        return numLogicalProcessors;
    }
#else
    CT_ASSERT(false);
#endif
    return 1;
}

#ifdef THREAD_TOOLS_DEBUG

struct DebugThreadInfo
{
	DebugThreadInfo()
	{
		myName[0]  = '\0';
		myThreadId = 0;
		mySemaWaiting = false;
		myCriticalWaiting = false;
	}
	char			myName[128];
	unsigned int	myThreadId;
	bool			mySemaWaiting;
	bool			myCriticalWaiting;
};

DebugThreadInfo locDebugThreadInfo[THREAD_TOOLS_ABSOLUTE_MAX_NUM_THREADS];

void MT_ThreadingTools::SetSemaphoreStatus( bool aWaiting)
{
	unsigned int idx = GetMyThreadIndex();
	locDebugThreadInfo[idx].mySemaWaiting = aWaiting;
}

void MT_ThreadingTools::SetCriticalSectionStatus( bool aWaiting)
{
	unsigned int idx = GetMyThreadIndex();
	locDebugThreadInfo[idx].myCriticalWaiting = aWaiting;
}



// Print status of all known threads
bool MT_ThreadingTools::PrintThreadList( char *aBuffer, unsigned int aBufferSize)
{
	if( aBuffer && aBufferSize > 0)
		aBuffer[0] = 0;

	char tmp[256];
	MC_String str;

	str += "\n------------------------------------------------------\n";
	str += "Thread status:\n";
	str += "   No.  Handle       ThreadID     cr se su bo pr   Name\n";
	str += "------------------------------------------------------\n";
	unsigned int currentThreadId = GetCurrentThreadId();

	for( int i=0; i<THREAD_TOOLS_ABSOLUTE_MAX_NUM_THREADS; i++)
	{
		if( locDebugThreadInfo[i].myThreadId && locDebugThreadInfo[i].myName[0] )
		{
			HANDLE threadhandle = OpenThread( THREAD_ALL_ACCESS, false, locDebugThreadInfo[i].myThreadId);
			if( threadhandle )
			{
				// Get suspend count by suspending and resuming.
				// It's important that we don't suspend the current thread!
				DWORD suspendcount = 0;
				if( locDebugThreadInfo[i].myThreadId != currentThreadId )
				{
					suspendcount = SuspendThread(threadhandle);
					if( suspendcount != (DWORD)-1)
						ResumeThread(threadhandle);
				}

				BOOL boost = false;
				int prio = GetThreadPriority( threadhandle);
				GetThreadPriorityBoost( threadhandle, &boost);

				char marker = ' ';
				if( locDebugThreadInfo[i].myThreadId == currentThreadId )
					marker = '*';

				sprintf(tmp, " %c %03d - 0x%p - 0x%08x -  %d  %d  %d  %d  %d - \"%s\"\n",
					marker,
					(unsigned int) i,
					threadhandle,
					(unsigned int) locDebugThreadInfo[i].myThreadId,
					(int) locDebugThreadInfo[i].myCriticalWaiting,
					(int) locDebugThreadInfo[i].mySemaWaiting,
					(int) suspendcount,
					(int) boost,
					(int) prio,
					locDebugThreadInfo[i].myName
				);

				CloseHandle(threadhandle);
				str += tmp;
			}
		}
	}

	// Dump to debug output
	MC_DEBUG(str);

	// Write back to buffer
	if( aBuffer && aBufferSize > 1)
	{
		unsigned int len = str.GetLength();
		if( len >= aBufferSize)
		{
			len = (aBufferSize-1);
			str[len] = 0;
		}
		strcpy( aBuffer, str);
	}

	return true;
}

extern MC_StackWalker* locSw;

// Print callstack for a thread
bool MT_ThreadingTools::PrintThreadCallStack( unsigned int aId, char *aBuffer, unsigned int aBufferSize)
{
	unsigned int currentThreadId = GetCurrentThreadId();
	if( locDebugThreadInfo[aId].myThreadId && locDebugThreadInfo[aId].myName[0] )
	{
		if( locDebugThreadInfo[aId].myThreadId != currentThreadId )
		{
			HANDLE threadhandle = OpenThread( THREAD_ALL_ACCESS, false, locDebugThreadInfo[aId].myThreadId);
			if( threadhandle)
			{
				MC_String str;

				str += "\n------------------------------------------------------\n";
				str += MC_Strfmt<256>("   %03d - \"%s\"\n", aId, locDebugThreadInfo[aId].myName);
				str += "------------------------------------------------------\n";


				if( SuspendThread(threadhandle) != DWORD(-1) )
				{
					MC_StaticString<8192> output = "";
					if( locSw )
						locSw->ShowCallstack(output, threadhandle);
					str += output;
					ResumeThread(threadhandle);
				}

				CloseHandle(threadhandle);

				// dump to debug output
				MC_DEBUG(str);

				// write back to buffer
				if( aBuffer && aBufferSize > 1)
				{
					unsigned int len = str.GetLength();
					if( len >= aBufferSize)
					{
						len = aBufferSize-1;
						str[len] = 0;
					}
					strcpy(aBuffer, str);
				}

				return true;
			}
		}
	}
	return false;
}

// Print status of all threads
bool MT_ThreadingTools::PrintThreadingStatus( bool aAllFlag, char *aBuffer, unsigned int aBufferSize)
{
	MC_String str;
	char tempstring[32 * 1024];
	memset(tempstring, 0, (32 * 1024) );

	// Thread list
	if( PrintThreadList(tempstring, 8191) )
	{
		str += tempstring;
	}

	// Callstack for all waiting threads
	for( int i=0; i<THREAD_TOOLS_ABSOLUTE_MAX_NUM_THREADS; i++)
	{
		if( aAllFlag || (locDebugThreadInfo[i].mySemaWaiting || locDebugThreadInfo[i].myCriticalWaiting) )
		{
			if( PrintThreadCallStack( i, tempstring, (32 * 1024) ))
			{
				str += tempstring;
			}
		}
	}

	// Write back to buffer
	if( aBuffer && aBufferSize > 1 )
	{
		unsigned int len = str.GetLength();
		if( len >= aBufferSize )
		{
			len = aBufferSize-1;
			str[len] = 0;
		}
		strcpy(aBuffer, str);
	}

	return true;
}


#endif //THREAD_TOOLS_DEBUG




void MT_ThreadingTools::SetCurrentThreadName(const char* aName)
{
#ifndef _RELEASE_
	typedef struct tagTHREADNAME_INFO
	{
		DWORD dwType; // must be 0x1000
		LPCSTR szName; // pointer to name (in user addr space)
		DWORD dwThreadID; // thread ID (-1=caller thread)
		DWORD dwFlags; // reserved for future use, must be zero
	} THREADNAME_INFO;

	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = aName;
	info.dwThreadID = (DWORD)-1;
	info.dwFlags = 0;

	__try{
		RaiseException(0x406D1388, 0, sizeof(info)/sizeof(DWORD), (ULONG_PTR*)&info);
	}
	__except (EXCEPTION_CONTINUE_EXECUTION)
	{
	}

#ifdef THREAD_TOOLS_DEBUG
	unsigned int idx = GetMyThreadIndex();
	strcpy( locDebugThreadInfo[idx].myName, aName);
	locDebugThreadInfo[idx].myThreadId = GetCurrentThreadId();
#endif

	MC_PROFILER_SET_THREAD_NAME(aName);
#endif
}


static MC_THREADLOCAL long locMyThreadIndex = -1;
static volatile long locThreadCount = 0;

int MT_ThreadingTools::GetMyThreadIndex()
{
	if(locMyThreadIndex == -1)
	{
		locMyThreadIndex = Increment(&locThreadCount)-1;

        MC_ASSERT(locMyThreadIndex >= 0 && locMyThreadIndex < THREAD_TOOLS_ABSOLUTE_MAX_NUM_THREADS);
	}

	return locMyThreadIndex;
}

long MT_ThreadingTools::Increment(long volatile* aValue)
{
	return _InterlockedIncrement(aValue);
}

long MT_ThreadingTools::Decrement(long volatile* aValue)
{
	return _InterlockedDecrement(aValue);
}

long MT_ThreadingTools::Exchange(long volatile* aTarget, long aValue)
{
    return _InterlockedExchange(aTarget, aValue);
}

long long MT_ThreadingTools::Exchange(long long volatile* aTarget, long long aValue)
{
	return _InterlockedExchange64(aTarget, aValue);
}

void* MT_ThreadingTools::Exchange(void* volatile* aTarget, void* aValue)
{
#if _WIN64
	return (void*)Exchange((long long volatile*)aTarget, (long long)aValue);
#elif _WIN32
	return (void*)Exchange((long volatile*)aTarget, (long)aValue);
#endif
}

long MT_ThreadingTools::ExchangeAdd(long volatile* anAddend, long aValue)
{
    return _InterlockedExchangeAdd(anAddend, aValue);
}

long MT_ThreadingTools::CompareExchange(long volatile* aDestination, long anExchange, long aComparand)
{
    return _InterlockedCompareExchange(aDestination, anExchange, aComparand);
}

long MT_ThreadingTools::Or(long volatile* aValue, long aMask)
{
    return _InterlockedOr(aValue, aMask);
}

long MT_ThreadingTools::And(long volatile* aValue, long aMask)
{
    return _InterlockedAnd(aValue, aMask);
}
