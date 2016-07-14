/*=========================================================================

  Program:   Open IGT Link -- Example for Tracking Data Server
  Module:    $RCSfile: $
  Language:  C++
  Date:      $Date: $
  Version:   $Revision: $

  Copyright (c) Insight Software Consortium. All rights reserved.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notices for more information.

=========================================================================*/

#include <fstream>
#include "igtlMessageDebugFunction.h"
//#include <pcl/visualization/pcl_visualizer.h>

#include "igtlOSUtil.h"
#include "igtlMessageHeader.h"
#include "igtlPolyDataMessage.h"
#include "igtlServerSocket.h"
#include "igtlMultiThreader.h"
#include "igtlConditionVariable.h"
#include <math.h>

#define IGTL_IMAGE_HEADER_SIZE          72
bool Synchonize = false;
namespace DepthImageServerForPolyData {
  void* ThreadFunction(void* ptr);
  typedef struct {
    igtl::MutexLock::Pointer glock;
    int   stop;
    int   portNum;
    bool transmissionFinished;
    std::vector<igtlFloat32> points;
    std::vector<RGBQUAD> pointsColor;
    igtl::ConditionVariable::Pointer conditionVar;
  } ThreadDataServer;

  typedef struct {
    int   nloop;
    igtl::MutexLock::Pointer glock;
    igtl::Socket::Pointer socket;
    int   interval;
    int   stop;
    ThreadDataServer* td_Server;
  } ThreadData;

  std::string     polyFile = "";
  
  void* ServerControl(void* ptr)
  {
    //------------------------------------------------------------
    // Parse Arguments
    igtl::MultiThreader::ThreadInfo* info =
      static_cast<igtl::MultiThreader::ThreadInfo*>(ptr);
    ThreadDataServer* tdServer = static_cast<ThreadDataServer*>(info->UserData);
    igtl::MutexLock::Pointer glockServer = tdServer->glock;
    int    port = tdServer->portNum;

    igtl::ServerSocket::Pointer serverSocket;
    serverSocket = igtl::ServerSocket::New();
    int r = serverSocket->CreateServer(port);

    if (r < 0)
    {
      std::cerr << "Cannot create a server socket." << std::endl;
      exit(0);
    }

    igtl::MultiThreader::Pointer threader = igtl::MultiThreader::New();
    igtl::MutexLock::Pointer glock = igtl::MutexLock::New();
    ThreadData td;
    td.td_Server = tdServer;

    while (1)
    {
      if (1/*!tdServer->stop*/)
      {
        //------------------------------------------------------------
        // Waiting for Connection
        int threadID = -1;
        igtl::Socket::Pointer socket;
        socket = serverSocket->WaitForConnection(1000);

        if (socket.IsNotNull()) // if client connected
        {
          std::cerr << "A client is connected." << std::endl;

          // Create a message buffer to receive header
          igtl::MessageHeader::Pointer headerMsg;
          headerMsg = igtl::MessageHeader::New();
          //------------------------------------------------------------
          // loop
          for (;;)
          {
            // Initialize receive buffer
            headerMsg->InitPack();

            // Receive generic header from the socket
            int rs = socket->Receive(headerMsg->GetPackPointer(), headerMsg->GetPackSize());
            if (rs == 0)
            {
              if (threadID >= 0)
              {
                td.stop = 1;
                threader->TerminateThread(threadID);
                threadID = -1;
              }
              std::cerr << "Disconnecting the client." << std::endl;
              td.socket = NULL;  // VERY IMPORTANT. Completely remove the instance.
              socket->CloseSocket();
              break;
            }
            if (rs != headerMsg->GetPackSize())
            {
              continue;
            }

            // Deserialize the header
            headerMsg->Unpack();

            // Check data type and receive data body
            if (strcmp(headerMsg->GetDeviceType(), "GET_POLYDATA") == 0)
            {
              std::cerr << "Received a GET_POLYDATA message." << std::endl;

              igtl::GetPolyDataMessage::Pointer getPolyDataMsg;
              getPolyDataMsg = igtl::GetPolyDataMessage::New();
              getPolyDataMsg->SetMessageHeader(headerMsg);
              getPolyDataMsg->AllocatePack();

              socket->Receive(getPolyDataMsg->GetPackBodyPointer(), getPolyDataMsg->GetPackBodySize());
              int c = getPolyDataMsg->Unpack(1);
              if (c & igtl::MessageHeader::UNPACK_BODY) // if CRC check is OK
              {
                td.glock = glock;
                td.socket = socket;
                td.stop = 0;
                threadID = threader->SpawnThread((igtl::ThreadFunctionType) &ThreadFunction, &td);
              }
            }
            else if (strcmp(headerMsg->GetDeviceType(), "STP_POLYDATA") == 0)
            {
              socket->Skip(headerMsg->GetBodySizeToRead(), 0);
              std::cerr << "Received a STP_POLYDATA message." << std::endl;
              if (threadID >= 0)
              {
                td.stop = 1;
                threader->TerminateThread(threadID);
                threadID = -1;
                std::cerr << "Disconnecting the client." << std::endl;
                td.socket = NULL;  // VERY IMPORTANT. Completely remove the instance.
                socket->CloseSocket();
              }
              break;
            }
            else
            {
              std::cerr << "Receiving : " << headerMsg->GetDeviceType() << std::endl;
              socket->Skip(headerMsg->GetBodySizeToRead(), 0);
            }
          }
        }
      }
      else
      {
        serverSocket->CloseSocket();
      }
    }

    //------------------------------------------------------------
    // Close connection (The example code never reaches to this section ...
  }

  void* ThreadFunction(void* ptr)
  {
    //------------------------------------------------------------
    // Get thread information
    igtl::MultiThreader::ThreadInfo* info =
      static_cast<igtl::MultiThreader::ThreadInfo*>(ptr);

    //int id      = info->ThreadID;
    //int nThread = info->NumberOfThreads;
    ThreadData* td = static_cast<ThreadData*>(info->UserData);

    //------------------------------------------------------------
    // Get user data
    igtl::MutexLock::Pointer glock = td->glock;
    long interval = td->interval;
    std::cerr << "Interval = " << interval << " (ms)" << std::endl;
    igtl::Socket::Pointer& socket = td->socket;

    //------------------------------------------------------------
    // Allocate TrackingData Message Class
    //
    // NOTE: TrackingDataElement class instances are allocated
    //       before the loop starts to avoid reallocation
    //       in each image transfer.
    while (!td->stop)
    {
      int iFrameIdx = 0;
      while ((!td->td_Server->transmissionFinished) && !td->stop)
      {
        //TestDebugCharArrayCmp(info.sLayerInfo[0].pBsBuf, info.sLayerInfo[0].pBsBuf, 200);
        //---------------
        igtl::PolyDataMessage::Pointer polyMsg;
        polyMsg = igtl::PolyDataMessage::New();
        // Create point array
        igtl::PolyDataPointArray::Pointer pointArray;
        pointArray = igtl::PolyDataPointArray::New();
        igtl::PolyDataAttribute::Pointer attribute;
        attribute = igtl::PolyDataAttribute::New();
        attribute->SetType(igtl::PolyDataAttribute::POINT_RGBA);
        attribute->SetName("attr");
        attribute->SetSize(td->td_Server->pointsColor.size());
        polyMsg->ClearAttributes();
        for (unsigned int i = 0; i < td->td_Server->pointsColor.size(); i++)
        {
          for (int j = 0; j < 3; j++)
          {
            pointArray->AddPoint(&(td->td_Server->points[i*3+j]));
          }
          igtlFloat32 colorData[3] = { td->td_Server->pointsColor[i].rgbRed, td->td_Server->pointsColor[i].rgbGreen,td->td_Server->pointsColor[i].rgbBlue };
          attribute->SetNthData(i, colorData);
        }
        polyMsg->SetPoints(pointArray);
        polyMsg->AddAttribute(attribute);
        polyMsg->Pack();
        glock->Lock();
        socket->Send(polyMsg->GetPackPointer(), polyMsg->GetPackSize());
        glock->Unlock();
        }
        td->td_Server->transmissionFinished = true;
        td->td_Server->conditionVar->Signal();
        //igtl::Sleep(interval);
      //------------------------------------------------------------
      // Loop
    }
    return NULL;
  }
}


