/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2019 National Instruments
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Vincent Kotzsch <vincent.kotzsch@ni.com>
 *         Clemens Felber <clemens.felber@ni.com>
 */

#include <iostream>
#include <sstream>
#include <fstream>
#include <semaphore.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <limits>

#include "ns3/system-thread.h"

#include "ns3/ni-utils.h"
#include "ni-logging.h"


namespace ns3 {

  NiLogging g_NiLogging;

  // trace variables
  uint64_t g_logTraceStartSubframeTime; // used for timing measurements

  NiLogging::NiLogging(void)
  {
    m_logIsEnable=false;
    m_fileOut="";
    m_isFirstCall = true;
    m_syncToFileInstant = false;
    m_loglevelMask = LOG__NONE;
    m_curLogBufferEntry = 0;
    m_logThreadPriority = 0;
  }

  NiLogging::~NiLogging(void)
  {

  }

  void
  NiLogging::Initialize (uint32_t loglevelMask, std::string fileName, int NiLoggingPriority)
  {
    if(pthread_mutex_init (&m_logMutex, NULL) != 0)
      {
        std::cout <<"\nNiLogging::Initialize: Error calling pthread_mutex_init for log stream " << std::endl;
      }
    if(sem_init (&m_semMsgCount, 0, 0) != 0)
      {
        NS_FATAL_ERROR("\nNiLogging::Initialize: Error calling sem_init!");
      }

    if (loglevelMask == LOG__NONE)
      {
        m_logIsEnable = false;
      }
    else
      {
        //TODO add more log levels
        m_logIsEnable = true;
      }
    m_loglevelMask = loglevelMask;

    m_fileOut = fileName;
    m_isFirstCall = true;
    m_filePtr.open(m_fileOut.c_str(), std::ios::out);
    m_filePtr << this->PrintHeader();
    gettimeofday(&m_firstCallSysTime, NULL);

    m_flagStopWriteThread = false;

    std::cout << "Start logging thread to file " << fileName << std::endl;
    m_logThreadPriority = NiLoggingPriority;
    m_logThread = Create<SystemThread> (MakeCallback (&NiLogging::writeThread, this));
    m_logThread->Start ();
  }

  void
  NiLogging::DeInitialize (void)
  {
    terminateWriteThread();
    WriteToFile();
    pthread_mutex_destroy(&m_logMutex);
    sem_destroy(&m_semMsgCount);
  }

  //Disable first call in write() function!!
  void
  NiLogging::DisableFirstCall(void)
  {
    m_isFirstCall = false;
  }

  //Thread which writes the log strings to file or buffer
  void
  NiLogging::writeThread()
  {
    NI_LOG_DEBUG("NI.LOGGING: logging thread with id:" << pthread_self() << " started");
    NiUtils::AddThreadInfo (pthread_self(), "Nilogging thread");

    // Main thread that writes the buffers into the log file
    // Now we reduce the priority of this thread
    NiUtils::SetThreadPrioriy(m_logThreadPriority);

    const uint32_t numMsgflushCnsl = 4; // after this number of log messages the console will be flushed
    uint32_t countMsgCnsl = 0;
    while(true)
      {
        niLogInfo logInfo;
        std::stringstream msgBufferStream;
        std::string timingInfo, sourceInfo, funcInfo;

        if (sem_wait (&m_semMsgCount) < 0)
          {
            NS_FATAL_ERROR("\nNiLogging:writeThread::Error calling sem_wait!");
          }
        if(m_flagStopWriteThread)
          {
            //std::cout << "Terminating logging thread...\n" <<std::endl;
            return;
          }
        if (pthread_mutex_lock (&m_logMutex) != 0)
          {
            NS_FATAL_ERROR("\nNiLogging::writeThread::Error calling pthread_mutex_lock!");
          }
        // get logging element from the queue
        if(m_msgQueue.size() == 0)
          {
            NS_FATAL_ERROR("\nNiLogging::writeThread::msgQueue length is zero!");
          }
        else
          {
            logInfo = m_msgQueue.front();
            m_msgQueue.pop_front();
          }
        if (pthread_mutex_unlock (&m_logMutex) != 0)
          {
            NS_FATAL_ERROR("\nNiLogging::writeThread::Error calling pthread_mutex_lock!");
          }

        funcInfo = ", " + logInfo.func + "(), ";
        timingInfo = ", Sim(us)=" + std::to_string(logInfo.simTimeUs)  +  ", Sys(us)=" + std::to_string(logInfo.sysTimeUs);

        if (logInfo.logLevel & m_loglevelMask)
          {
            switch (logInfo.logLevel) {
              case LOG__NONE:
                break;
              case LOG__FATAL:
                msgBufferStream << "[FATAL]" << timingInfo << sourceInfo << ", " << logInfo.buffer;
                break;
              case LOG__ERROR:
                msgBufferStream << "[ERROR]" << timingInfo << funcInfo << logInfo.buffer;
                break;
              case LOG__WARN:
                msgBufferStream << "[WARN ]" << timingInfo << funcInfo << logInfo.buffer;
                break;
              case LOG__INFO:
                msgBufferStream << "[INFO ]" << timingInfo << funcInfo << logInfo.buffer;
                break;
              case LOG__DEBUG:
                msgBufferStream << "[DEBUG]" << timingInfo << funcInfo << logInfo.buffer;
                break;
              case LOG__TRACE:
                msgBufferStream << "[TRACE]" << ", " << logInfo.simTimeUs << "," << logInfo.sysTimeUs << ", " << logInfo.buffer;
                break;
              case LOG__CONSOLE_DEBUG:
                msgBufferStream << "[CNSL ]" << timingInfo << funcInfo << logInfo.buffer;
                std::cout << logInfo.buffer << std::endl;
                if (countMsgCnsl % numMsgflushCnsl) fflush( stdout );
                countMsgCnsl++;
                break;
              default:
                NS_FATAL_ERROR("\nNiLogging::writeThread:: Error undefined loglevel:" << logInfo.logLevel);
                break;
            }

            // Now we write to file pointer
            if (m_syncToFileInstant)
              {
                // write to file
                m_filePtr << msgBufferStream.str() << std::endl;
              }
            else
              {
                // write into circular buffer
                m_logStringBuffer[m_curLogBufferEntry].str(std::string()); // clear the stringstream
                m_logStringBuffer[m_curLogBufferEntry] << msgBufferStream.str() << std::endl;
                m_curLogBufferEntry =  (m_curLogBufferEntry+1) % NI_LOG__BUFFER_SIZE;
              }
          }
      }
  }

  void
  NiLogging::Write (const enum NiLogLevel logLevel, const std::string file, const int line, const std::string func, const std::string &buffer)
  {
    if(buffer.length() <= 0)
      {
        return; //No need to log anything if the message field is zero
      }

    struct timeval systime, timeDiff;
    uint64_t simTimeUs, sysTimeUs;
    niLogInfo logInfo;

    // get system time
    gettimeofday(&systime, NULL);
    // get simulator time
    simTimeUs = Simulator::Now().GetMicroSeconds();

    //First call to write function
    if(m_isFirstCall)
      {
        m_isFirstCall = false;
        m_firstCallSysTime = systime;
      }

    // calc normalized system timing in us
    timersub(&systime, &m_firstCallSysTime, &timeDiff);
    sysTimeUs = timeDiff.tv_sec * 1000000 + timeDiff.tv_usec;

    logInfo.simTimeUs = simTimeUs;
    logInfo.sysTimeUs = sysTimeUs;
    logInfo.logLevel = logLevel;
    logInfo.file = file;
    logInfo.line = line;
    logInfo.func = func;
    logInfo.buffer = buffer;

    // put messages to logfile
    if (pthread_mutex_lock (&m_logMutex) != 0)
      {
        NS_FATAL_ERROR("\nNiLogging::Write::Error calling pthread_mutex_lock!");
      }

    m_msgQueue.push_back(logInfo);

    if (pthread_mutex_unlock (&m_logMutex) != 0)
      {
        NS_FATAL_ERROR("\nNiLogging::Write::Error calling pthread_mutex_lock!");
      }
    if (sem_post (&m_semMsgCount) < 0)
      {
        NS_FATAL_ERROR("\nNiLogging::Write::Error calling sem_post!");
      }

    // stop system on FATAL ERROR and print out debugging info
    if (logInfo.logLevel == LOG__FATAL)
      {
        std::string funcInfo = ", " + logInfo.func + "(), ";
        std::string timingInfo = ", Sim(us)=" + std::to_string(logInfo.simTimeUs)  +  ", Sys(us)=" + std::to_string(logInfo.sysTimeUs);
        std::string sourceInfo = ", " + logInfo.file + ", line " + std::to_string(logInfo.line) + funcInfo;
        // print out to stdout
        std::cout << std::endl << "[FATAL]" << timingInfo << sourceInfo << ", " << logInfo.buffer << std::endl << std::endl;
        fflush( stdout );
        // stop logging
        NiLoggingDeInit();
        struct timespec ts = {1,0};
        nanosleep( &ts, NULL );
        // print call stack
        NiUtils::Backtrace();
        // stop NS-3
        NS_FATAL_ERROR ("NI_LOG_FATAL"); // this also stops the whole application
      }
  }

  void
  NiLogging::terminateWriteThread()
  {
    m_flagStopWriteThread = true;

    if(sem_post (&m_semMsgCount) < 0)
      {
        NS_FATAL_ERROR("\nLteSpectrumPhy::terminateWriteThread::Error calling sem_post!");
      }
    m_logThread->Join();
  }

  void
  NiLogging::WriteToFile()
  {

    if (m_syncToFileInstant == 0)
      {
        // handle buffer wrap and write to file
        for(int i = m_curLogBufferEntry;i < NI_LOG__BUFFER_SIZE; i++){
            m_filePtr << m_logStringBuffer[i].str();
        }
        for(int i = 0; i < m_curLogBufferEntry; i++){
            m_filePtr << m_logStringBuffer[i].str();
        }
      }

    m_filePtr.close();

  }

  bool
  NiLogging::IsEnable(void)
  {
    return m_logIsEnable;
  }

  // Synchronize log files instantly ...disable synchronization for better real-time behavior"
  void
  NiLogging::EnableSyncToFileInstant (void)
  {
    m_syncToFileInstant = true;
  }

  // Synchronize log files instantly ...disable synchronization for better real-time behavior"
  const std::string
  NiLogging::PrintHeader (void)
  {
    std::stringstream msgBufferStream;
    time_t now = time(0);
    char *curTime = ctime(&now);
    if (curTime[strlen(curTime)-1] == '\n') curTime[strlen(curTime)-1] = '\0';
    msgBufferStream << "=======  NI Log file, " << curTime << ", NS-3 Simulator Resolution = " << Simulator::Now().GetResolution() << "  =======" << std::endl;
    return msgBufferStream.str();
  }

} // namespace ns3

