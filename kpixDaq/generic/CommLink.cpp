// File          : CommLink.cpp
// Author        : Ryan Herbst  <rherbst@slac.stanford.edu>
// Created       : 04/12/2011
// Project       : General Purpose
//-----------------------------------------------------------------------------
// Description :
// Generic communications link
//-----------------------------------------------------------------------------
// This file is part of 'SLAC Generic DAQ Software'.
// It is subject to the license terms in the LICENSE.txt file found in the 
// top-level directory of this distribution and at: 
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html. 
// No part of 'SLAC Generic DAQ Software', including this file, 
// may be copied, modified, propagated, or distributed except according to 
// the terms contained in the LICENSE.txt file.
// Proprietary and confidential to SLAC.
//-----------------------------------------------------------------------------
// Modification history :
// 04/12/2011: created
//-----------------------------------------------------------------------------

#include <CommLink.h>
#include <Register.h>
#include <Command.h>
#include <Data.h>
#include <CommQueue.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <DataSharedMem.h>
#include <arpa/inet.h>
using namespace std;
#include <sys/socket.h>

bool wmqInComm = false;

// IO Thread
void * CommLink::ioRun ( void *t ) {
   CommLink *ti;
   ti = (CommLink *)t;
   ti->ioHandler();
   pthread_exit(NULL);
   return(NULL);
}

// RX Thread
void * CommLink::rxRun ( void *t ) {
   CommLink *ti;
   ti = (CommLink *)t;
   ti->rxHandler();
   pthread_exit(NULL);
   return(NULL);
}

// Data Thread
void * CommLink::dataRun ( void *t ) {
   CommLink *ti;
   ti = (CommLink *)t;
   ti->dataHandler();
   pthread_exit(NULL);
   return(NULL);
}

// Dummy IO routine
void CommLink::ioHandler() {
   uint32_t      lastReqCnt;
   uint32_t      lastCmdCnt;
   uint32_t      lastDataCnt;
   uint32_t      lastRunCnt;

   lastReqCnt  = regReqCnt_;
   lastCmdCnt  = cmdReqCnt_;
   lastDataCnt = dataReqCnt_;
   lastRunCnt  = runReqCnt_;

   while ( runEnable_ ) {
      if ( lastReqCnt != regReqCnt_ ) {
         lastReqCnt = regReqCnt_;
         regRespCnt_++;
         mainThreadWakeup();
      }
      if ( lastCmdCnt != cmdReqCnt_ ) {
         lastCmdCnt = cmdReqCnt_;
         cmdRespCnt_++;
         mainThreadWakeup();
      }
      if ( lastDataCnt != dataReqCnt_ ) {
         lastDataCnt = dataReqCnt_;
         dataRespCnt_++;
         mainThreadWakeup();
      }      
      if ( lastRunCnt != runReqCnt_ ) lastRunCnt = runReqCnt_;
      ioThreadWait(1000);
   }
}

// Dummy RX routine
void CommLink::rxHandler() { }


// Data routine
void CommLink::dataHandler() {
   Data      *dat;
   uint32_t   size;
   uint32_t * buff;
   int32_t    slen = sizeof(net_addr_);
   time_t    ltime;
   time_t    ctime;
   uint32_t   xmlCount;
   uint32_t   xmlSize;
   uint32_t   wrSize;
   bool       idle;

#ifdef USE_BZLIB
   int32_t     bzerror;
#endif
   if (wmqInComm) cout<<"\t[CommLink:dev] dataHandler() start! with runEnable_ =="<<runEnable_<<endl;
   // Store time
   time(&ltime);
   ctime        = ltime;
   dataRxCount_ = 0;
   xmlCount     = xmlReqCnt_;

   // Running
   while ( runEnable_ ) {
      idle = true;
      //cout<< "[CommLink:dev] COUNTER = " << dataRxCount_ <<endl;
      // Config/status/start/stop update
      if ( xmlCount != xmlReqCnt_ ) {
	if (wmqInComm) cout<<"[CommLink:dev] dataHandler() update since config/status/start/stop changed ==> file:\n    "<< dataFileFd_<<endl;
	//cout<<"[CommLink:debug] dat=="<<&dat<<endl;
	
         // Storing is enabled
         if ( xmlStoreEn_ ) {
            wrSize = (xmlReqEntry_.length() & 0x0FFFFFFF);
            xmlSize = ((xmlType_ << 28) & 0xF0000000) | wrSize;

            // Callback function is set
            if ( dataCb_ != NULL ) dataCb_((void *)xmlReqEntry_.c_str(), xmlSize);
	    
	    /*// Network is open
            if ( dataNetFd_ >= 0 ) {
               sendto(dataNetFd_,&xmlSize,4,0,(const sockaddr*)&net_addr_,slen);
	       // write(1, &xmlSize,4 );
	       sendto(dataNetFd_,xmlReqEntry_.c_str(),wrSize,0,(const sockaddr*)&net_addr_,slen);
	       uint32_t tail_=0xFFFFFFFF;
	       sendto(dataNetFd_,&tail_,4,0,(const sockaddr*)&net_addr_,slen);
	       // write(1, "\t=> wrSize = ",13);
	       // write(1, &wrSize, 4);
	       // write(1, "\n", 1);
	       // write(1, xmlReqEntry_.c_str(), wrSize);
	       }*/
	    
	    /*Start -- mengqing dev
	    * aim: to fix the oversized buffer via udp
	    */
	    if ( dataNetFd_ >=0 ){
	      sendto(dataNetFd_,&xmlSize,4,0,(const sockaddr*)&net_addr_,slen);
	      uint32_t bufferSize = 1024;
	      uint32_t messageLength = wrSize;
	      int sendPos = 0;
	      char *buffer;
	      const char* source = xmlReqEntry_.c_str();
	      while (messageLength){
		int chunkSize = messageLength > bufferSize ? bufferSize : messageLength;
		buffer = new char[chunkSize];

		memcpy(buffer, source + sendPos, chunkSize);
		//write(1, buffer, chunkSize);
		//write(1, "\n==\n", 1);

		chunkSize = sendto(dataNetFd_, buffer, chunkSize, 0, (const sockaddr*)&net_addr_, slen);
		messageLength -= chunkSize;
		sendPos += chunkSize;
	      }
	      uint32_t tail_=0xFFFFFFFF;
	      sendto(dataNetFd_,&tail_,4,0,(const sockaddr*)&net_addr_,slen);
	    }
	    /*End -- mengqing dev*/
	    
            // Shared memory
            if ( smem_ != NULL ) 
               dataSharedWrite ((DataSharedMemory *)smem_, xmlSize, (const uint8_t *)xmlReqEntry_.c_str(), wrSize );

	    // Data file in compressed mode
            if ( bzEnable_ ) {
#ifdef USE_BZLIB
               BZ2_bzWrite(&bzerror,bzFile_,&xmlSize,4);
               BZ2_bzWrite(&bzerror,bzFile_,(void *)xmlReqEntry_.c_str(),wrSize);
#endif
            }

            // Data file is open
            else if ( dataFileFd_ >= 0 ) {
	      // cout<<"[CommLink:dev] dataHandler() says 'data file is open and now is writing to it!' ==> file:\n    "<< dataFileFd_<<endl;
	      
               write(dataFileFd_,&xmlSize,4);
               write(dataFileFd_,xmlReqEntry_.c_str(),wrSize);
            }
         }
         xmlCount = xmlReqCnt_;
         xmlRespCnt_++;
         idle = false;
      }
      
      // Data is ready
      if ( (dat = (Data *)dataQueue_.pop()) != NULL ) {
         size = dat->size();
         buff = dat->data();

	 /*Nov22 wMq*/
	 if (eudaqPush_){
	   Data *dat_toread = new Data( buff, size);
	   std::unique_lock<std::mutex> mylock(eudaqQueue_guard);
	   eudaqQueue_.push(dat_toread);
	   mylock.unlock();
	 }
	 /*Nov22 wMq*/

         // Callback function is set
         if ( dataCb_ != NULL ) dataCb_(buff, size);
	 
         /*// Network is open
         if ( dataNetFd_ >= 0 ) {
            sendto(dataNetFd_,&size,4,0,(const sockaddr*)&net_addr_,slen);
            sendto(dataNetFd_,buff,size*4,0,(const sockaddr*)&net_addr_,slen);
	    uint32_t tail_=0xFFFFFFFF;
	    sendto(dataNetFd_,&tail_,4,0,(const sockaddr*)&net_addr_,slen);
	    }*/

	 /*Start -- mengqing dev
	  * aim: to fix the oversized buffer via udp
	  */
	 if ( dataNetFd_ >= 0 ) {
	   cout<<"[rawData] how long I am? => "<<std::hex << size<<endl;
	   sendto(dataNetFd_,&size,4,0,(const sockaddr*)&net_addr_,slen);
	   
	   uint32_t bufferSize = 1024/sizeof(uint32_t); // should be 1024/4 = 256
	   uint32_t msgLength = size;

	   int sendPos = 0;
	   uint32_t *buffchunk;
	   while(msgLength){
	     uint32_t chunkSize = msgLength > bufferSize ? bufferSize : msgLength;
	     buffchunk = new uint32_t[chunkSize];
	     
	     memcpy( buffchunk, buff+sendPos, chunkSize*4);
	     //cout << hex<< buffchunk[0]<<" ";
	     sendto(dataNetFd_,buffchunk,chunkSize*4,0,(const sockaddr*)&net_addr_,slen);
	     
	     msgLength -= chunkSize;
	     sendPos += chunkSize;
	   }
	   //sendto(dataNetFd_,buff,size*4,0,(const sockaddr*)&net_addr_,slen);
	   
	   uint32_t tail_=0xFFFFFFFF;
	   sendto(dataNetFd_,&tail_,4,0,(const sockaddr*)&net_addr_,slen);
	 }
	 /*End -- mengqing dev*/
	 
	 
         // Shared memory
         if ( smem_ != NULL ) 
            dataSharedWrite ((DataSharedMemory *)smem_, size, (uint8_t *)buff, size*4 );

         // Data file in compressed mode
         if ( bzEnable_ ) {
#ifdef USE_BZLIB
            BZ2_bzWrite(&bzerror,bzFile_,&size,4);
            BZ2_bzWrite(&bzerror,bzFile_,buff,size*4);
            dataFileCount_++;
#endif
         }

         // File is open
         else if ( dataFileFd_ >= 0 ) {
	   //cout<< " [CommLink:dev] ^_^ file is open, and data buffer to write on Fd_ ;)"<<endl;
	   if (wmqInComm) std::cout<<"[CommLink.CHECK] buff = "<< buff << "\n"
				   <<"                 size = "<< size << "\n"
				   <<"                 &size= "<< &size<< std::endl;
	   
            write(dataFileFd_,&size,4);
            write(dataFileFd_,buff,size*4);
            dataFileCount_++;
         }
         dataRxCount_++;
         delete dat;

         // Debug once a second
         if ( debug_ ) {
	   time(&ctime);
	   if ( ltime != ctime ) {
	     cout << "CommLink::dataHandler -> Received data. Size = " << dec << size
		  << ", TotCount = " << dec << dataRxCount_
		  << ", FileCount = " << dec << dataFileCount_ 
		  << ", Buffer Depth = " << dec << dataQueue_.entryCnt() << endl;
	     ltime = ctime;
	   }
         }
         idle = false;
	 }
      //-- wmq
      
      if ( idle ) dataThreadWait(1000);
   }
}

Data* CommLink::pollEudaqQueue(){
  std::unique_lock<std::mutex> eulock(eudaqQueue_guard);
  if (eudaqQueue_.empty()) return NULL;
  else {
    cout << "[commlink] eudaq queue size is"<< eudaqQueue_.size()<<endl;
    auto toreturn = eudaqQueue_.front();
    eudaqQueue_.pop();
    eulock.unlock();
    return toreturn;
  }
  /* REMINDER: always delete this pointer after get it by calling this function!*/
}

//! Function for polling the queue when the RX Thread is disabled
Data* CommLink::pollDataQueue(uint32_t wait) {
   Data *dat;   
   //cout<< "[CommLink] I am testing @_@" <<endl;//wmq
   // Check if RxThread is disabled
   if(!enDataThread_){
     // Get the data from the queue
      dat = (Data *)dataQueue_.pop(wait);
      //if (dat==NULL) cout<<"CommLink->pollDataHandler(): empty Data from buffer ==> CHECK! TAT" <<endl;
   } else {
     cout << "CommLink->pollDataHandler(): No operation because RxThread is enabled" << endl;
   }
   return dat;
}

// Constructor
CommLink::CommLink ( ) {
  eudaqPush_ = false;
   debug_           = false;
   dataSource_      = 0;
   dataFileFd_      = -1;
   dataFile_        = "";
   dataNetFd_       = -1;
   dataNetAddress_  = "";
   dataNetPort_     = 0;
   dataRespCnt_     = 0;
   regReqEntry_     = NULL;
   regReqConf_      = 0;
   regReqCnt_       = 0;
   regReqWrite_     = false;
   regRespCnt_      = 0;
   cmdReqEntry_     = NULL;
   cmdReqConf_      = 0;
   cmdReqCnt_       = 0;
   cmdRespCnt_      = 0;
   runReqEntry_     = NULL;
   runReqConf_      = 0;
   runReqCnt_       = 0;
   runEnable_       = false;
   dataFileCount_   = 0;
   dataRxCount_     = 0;
   regRxCount_      = 0;
   timeoutCount_    = 0;
   errorCount_      = 0;
   maxRxTx_         = 4;
   dataCb_          = NULL;
   unexpCount_      = 0;
   xmlReqEntry_     = "";
   xmlType_         = 0;
   xmlReqCnt_       = 0;
   xmlRespCnt_      = 0;
   xmlStoreEn_      = true;
   toDisable_       = false;
   smem_            = NULL;
   bzEnable_        = false;

   pthread_mutex_init(&reqMutex_,NULL);
   pthread_mutex_init(&ioMutex_,NULL);
   pthread_mutex_init(&dataMutex_,NULL);
   pthread_mutex_init(&mainMutex_,NULL);

   pthread_cond_init(&ioCondition_,NULL);
   pthread_cond_init(&dataCondition_,NULL);
   pthread_cond_init(&mainCondition_,NULL);
}

// Deconstructor
CommLink::~CommLink () { 
   close();
   if ( smem_ != NULL ) dataSharedClose((DataSharedMemory*)smem_);
}

// Open link and start threads
void CommLink::open (bool enDataThread) {
   stringstream err;
   runEnable_ = true;
   enDataThread_ = enDataThread;

   err.str("");
   if (wmqInComm) cout<<"[CommLink:dev] open() called to start threads, enDataThread=="<<enDataThread<<endl;
   // Start io thread
   if ( pthread_create(&ioThread_,NULL,ioRun,this) ) {
      err << "CommLink::open -> Failed to create ioThread" << endl;
      if ( debug_ ) cout << err.str();
      close();
      throw(err.str());
   }
#ifdef ARM
   else pthread_setname_np(ioThread_,"cLinkIoThread");
#endif

   // Start rx thread
   if ( pthread_create(&rxThread_,NULL,rxRun,this) ) {
      err << "CommLink::open -> Failed to create rxThread" << endl;
      if ( debug_ ) cout << err.str();
      close();
      throw(err.str());
   }
#ifdef ARM
   else pthread_setname_np(rxThread_,"cLinkRxThread");
#endif

   if(enDataThread_) {
      // Start data thread
      if ( pthread_create(&dataThread_,NULL,dataRun,this) ) {
         err << "CommLink::open -> Failed to create dataThread" << endl;
         if ( debug_ ) cout << err.str();
         close();
         throw(err.str());
      }
   #ifdef ARM
      else pthread_setname_np(dataThread_,"cLinkDataThread");
   #endif
      usleep(1000); // Let threads catch up
   }
}

// Stop threads and close link
void CommLink::close () {

   // Stop the thread
   runEnable_ = false;

   // Wake up threads
   ioThreadWakeup();
   dataThreadWakeup();
   usleep(1100); // Give enough time to threads to stop

   // Wait for thread to stop
   pthread_join(ioThread_, NULL);
   pthread_join(rxThread_, NULL);
   if(enDataThread_) {
      pthread_join(dataThread_, NULL);
   }
}

// Open data file
void CommLink::openDataFile (string file, bool compress) {
   stringstream tmp;

#ifdef USE_BZLIB
   FILE         *f;
   int32_t    bzerror;
   bzEnable_ = compress;
#else
   bzEnable_ = false;
#endif

   dataFile_ = file;

   // Compress mode
   if ( bzEnable_ ) {
     
#ifdef USE_BZLIB
     //cout<<"[CommLink:dev] USE_BZLIB defined w/ Compress mode!"<<endl;
      dataFileFd_ = -1;

      // Open file
      umask(002);
      f = fopen ( file.c_str(), "a" );

      // Attempt to compress file
      if ( f ) {
         bzFile_ = BZ2_bzWriteOpen(&bzerror,f,9,0,30);
         if ( bzerror != BZ_OK ) bzEnable_ = false;
      }
      else bzEnable_ = false;

      // Status
      tmp.str("");
      tmp << "CommLink::openDataFile -> ";
      if ( dataFileFd_ < 0 ) tmp << "Error opening compressed data file ";
      else tmp << "Opened compressed data file ";
      tmp << file << endl;

      // Debug result
      if ( debug_ ) cout << tmp.str();
      dataFileCount_ = 0;

      // Status
      if ( ! bzEnable_ ) throw(tmp.str());
#endif
   }

   // Non compress mode
   else {
     cout<<"[CommLink:dev] non Compress mode!"<<endl;
      // Open the file
      dataFileFd_ = ::open(file.c_str(),O_RDWR|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);

      // Status
      tmp.str("");
      tmp << "CommLink::openDataFile -> ";
      if ( dataFileFd_ < 0 ) tmp << "Error opening data file ";
      else tmp << "Opened data file ";
      tmp << file << endl;

      // Debug result
      if ( debug_ ) cout << tmp.str();
      dataFileCount_ = 0;

      // Status
      if ( dataFileFd_ < 0 ) throw(tmp.str());
   }
}

// Close data file
void CommLink::closeDataFile () {
#ifdef USE_BZLIB
   int32_t  bzerror;
#endif

   // Close compression
   if ( bzEnable_ ) {
#ifdef USE_BZLIB
      bzEnable_ = false;
      BZ2_bzWriteClose(&bzerror,bzFile_,0,NULL,NULL);
#endif
   }
   else {
      ::close(dataFileFd_);
      dataFileFd_  = -1;
   }

   if ( debug_ ) {
      cout << "CommLink::closeDataFile -> "
           << "Closed data file " << dataFile_
           << ", Count = " << dec << dataFileCount_ << endl;
   }
   dataFileCount_ = 0;
}

// Open data network
void CommLink::openDataNet (string address, int32_t port) {
   stringstream dbg;

   std::cout<<"\t I am debugging: addr =  "<< address
	    <<",\t port = "<< std::dec<< port <<std::endl;
   dataNetAddress_ = address;
   dataNetPort_    = port;
   dbg.str("");

   // Create socket
   dataNetFd_ = socket(AF_INET,SOCK_DGRAM, IPPROTO_UDP); 
   if ( dataNetFd_ == -1 ) {
      dbg << "CommLink::openDataNet -> ";
      dbg << "Error creating UDP socket" << endl;
      if ( debug_ ) cout << dbg.str();
      throw(dbg.str());
   }
   
   // Setup address
   memset((char *)&net_addr_, 0, sizeof(net_addr_));
   net_addr_.sin_family = AF_INET;
   net_addr_.sin_port = htons(port);

//#ifndef RTEMS // FIX
   if ( inet_aton(address.c_str(),&net_addr_.sin_addr) == 0 ) {
      dbg << "CommLink::openDataNet -> ";
      dbg << "Error resolving UDP address: " << address << endl;
      if ( debug_ ) cout << dbg.str();
      dataNetFd_ = -1;
      throw(dbg.str());
   }
//#endif

   //--> start of wmq dev*****************************************
   bool wmqTest=true;
   if (wmqTest){
     int testmsg=0;
     unsigned int fromlen = sizeof(struct sockaddr_in);
     
     if ( bind(dataNetFd_, (struct sockaddr *)&net_addr_, sizeof(struct sockaddr_in))<0 )
       perror("binding");

     if (dataNetFd_>=0){
       puts("\n\tStart to send...\n");
       while(1){
	 //testmsg = recvfrom(dataNetFd_, buf, 1024, 0, (sockaddr*)&net_addr_, &fromlen);
	 //if (testmsg<0) perror("dataNetFd_:recvfrom");
	 //write(1,"Received a datagram: ",21);
	 //write(1,buf,testmsg);
	 //puts("\n\tStop listen and send hello *_* \n");
	 testmsg = sendto(dataNetFd_,"Greetings from KPiX\n",20,
			  0,(const sockaddr*)&net_addr_, fromlen);
	 if (testmsg<0) perror("dataNetFd_:sendto");
	 else break; 
       }
       puts("\n\tFinishing test the socket\n");
     }
   }
   //--> end of wmq dev*****************************************
   
   // Debug result
   if ( debug_ ) {
      cout << "CommLink::openDataNet -> Opened network connection " << address << endl;
   }
}

// Close data file
void CommLink::closeDataNet () {
  uint32_t _closing = 0xABABABAB;
  int32_t  fromlen = sizeof(net_addr_);
  sendto(dataNetFd_,&_closing,4,0,(const sockaddr*)&net_addr_, fromlen);
  
  ::close(dataNetFd_);
  dataNetFd_  = -1;
  cout<< "CLOSING NETWORK" <<endl;
   
  if ( debug_ ) {
    cout << "CommLink::closeData -> "
	 << "Closed data network " << dataNetAddress_ << endl;
  }
}

//! Set data callback function
void CommLink::setDataCb ( void (*dataCb)(void *, uint32_t)) {
   dataCb_ = dataCb;
}

// Set debug flag
void CommLink::setDebug( bool enable ) {
   debug_ = enable;
}

// Queue register request
void CommLink::queueRegister ( uint32_t linkConfig, Register *reg, bool write, bool wait ) {
   uint32_t       currResp;
   stringstream   err;
   uint32_t       tryCount;
   bool           error = false;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   if ( (reg->size()+3) > maxRxTx_ ) {
      err.str("");
      err << "CommLink::queueRegister -> Register: " << reg->name();
      err << "Register size exceeds maxRxTx!";
      if ( debug_ ) cout << err.str() << endl;
      error = true;
   }
   else {

      // Setup request
      regReqEntry_ = reg;
      regReqConf_  = linkConfig;
      regReqWrite_ = write;
      tryCount     = 0;

      do {
         currResp = regRespCnt_;
         regReqCnt_++;
         tryCount++;
         error = false;
         initTime(&tm);
         ioThreadWakeup();

         // Wait for response, 250mS timeout
         while ( wait && !error && currResp == regRespCnt_ ) {
            if ( timePassed(&tm,250000) && (!toDisable_)) { 
               err.str("");
               err << "CommLink::queueRegister -> Register: " << reg->name();
               if ( write ) err << ", Write"; else err << ", Read";
               err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
               err << ", Address: 0x" << hex << setw(8) << setfill('0') << reg->address();
               err << ", Attempt: " << dec << tryCount;
               err << ", Timeout, Trying Again!";
               cout << err.str() << endl;
               timeoutCount_++;
               error = true;
            }
            else mainThreadWait(100); // 100uS
         }

         if ( !error ) regRxCount_++;

         // Check status value
         if ( wait && !error && (reg->status() != 0) ) {
            err.str("");
            err << "CommLink::queueRegister -> Register: " << reg->name();
            if ( write ) err << ", Write"; else err << ", Read";
            err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
            err << ", Address: 0x" << hex << setw(8) << setfill('0') << reg->address();
            err << ", Status: 0x" << hex << setw(8) << setfill('0') << reg->status();
            err << ", Attempt: " << dec << tryCount;
            err << ", Status Error, Trying Again!";
            cout << err.str() << endl;
            error = true;
         }
         regRxCount_++;

      } while ( error && tryCount <= 5 );

      // Error occured
      if ( error ) {
         err.str("");
         err << "CommLink::queueRegister -> Register: " << reg->name();
         if ( write ) err << ", Write"; else err << ", Read";
         err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
         err << ", Address: 0x" << hex << setw(8) << setfill('0') << reg->address();
         err << ", Failed!!!!";
         cout << err.str() << endl;
      }
      else reg->clrStale();
   }
   pthread_mutex_unlock(&reqMutex_);

   if ( error ) throw(err.str());
}

// Queue command request
void CommLink::queueCommand ( uint32_t linkConfig, Command *cmd ) {
   uint32_t       currResp;
   stringstream   err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   cmdReqEntry_ = cmd;
   cmdReqConf_  = linkConfig;
   currResp     = cmdRespCnt_;
   cmdReqCnt_++;
   initTime(&tm);
   ioThreadWakeup();

   // Wait for response, 250mS timeout
   while ( currResp == cmdRespCnt_ ) {
      if ( timePassed(&tm,250000) && (!toDisable_)) {
         err.str("");
         err << "CommLink::queueCommand -> Command: " << cmd->name();
         err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
         err << ", OpCode: 0x" << hex << setw(4) << setfill('0') << cmd->opCode();
         err << ", Timeout!";
         if ( debug_ ) cout << err.str() << endl;
         timeoutCount_++;
         pthread_mutex_unlock(&reqMutex_);
         throw(err.str());
      } 
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}

// Queue data transmission
void CommLink::queueDataTx ( uint32_t linkConfig, uint32_t address, uint32_t *txBuffer, uint32_t txLength ) {
   uint32_t       currResp;
   uint32_t       i;
   stringstream   err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   dataReqEntry_  = txBuffer;
   dataReqLength_ = txLength;
   dataReqConf_   = linkConfig;
   dataReqAddr_   = address;
   currResp       = dataRespCnt_;
   dataReqCnt_++;
   initTime(&tm);
   ioThreadWakeup();

   //Check that exactly one lane and VC are selected
   uint32_t lane    = (dataReqAddr_ >> 4) & 0xF;
   uint32_t vc      = (dataReqAddr_     ) & 0xF;
   uint32_t laneSum = 0;
   uint32_t vcSum   = 0;
   for ( i = 0; i < 4; ++i ) {
      laneSum += (lane >> i) & 0x1;
      vcSum   += (vc   >> i) & 0x1;
   }

   if ( vcSum != 1 || laneSum != 1 ) {
         err.str("");
         err << "CommLink::queueDataTx -> ";
         err << " bad VC/lane selection: ";
         err << ", Lane: 0x" << hex << setw(8) << setfill('0') << lane;
         err << ", Vc: 0x" << hex << setw(8) << setfill('0') << vc;
         pthread_mutex_unlock(&reqMutex_);
         throw(err.str());

   }
   else if ( txLength > maxRxTx_ ) {
         err.str("");
         err << "CommLink::queueDataTx -> ";
         err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
         err << ", Size: " << dec << txLength;
         err << ", First word: " << hex << setw(8) << setfill('0') << txBuffer[0];
         err << ", Exceeded maxRxTx_: " << dec << maxRxTx_;
         pthread_mutex_unlock(&reqMutex_);
         throw(err.str());

   } else {
      // Wait for response, 250mS timeout
      while ( currResp == dataRespCnt_ ) {
         if ( timePassed(&tm,250000) && (!toDisable_)) {
            err.str("");
            err << "CommLink::queueDataTx -> ";
            err << ", LinkConfig: 0x" << hex << setw(8) << setfill('0') << linkConfig;
            err << ", Size: " << dec << txLength;
            err << ", First word: " << hex << setw(8) << setfill('0') << txBuffer[0];
            err << ", Timeout!";
            cout << err.str() << endl;
            //if ( debug_ ) cout << err.str() << endl;
            timeoutCount_++;
            pthread_mutex_unlock(&reqMutex_);
            throw(err.str());
         }
         mainThreadWait(100);
      }   
   }   
   pthread_mutex_unlock(&reqMutex_);
}

// Queue run command request
void CommLink::queueRunCommand ( ) {
   pthread_mutex_lock(&reqMutex_);
   if ( runReqEntry_ != NULL ) runReqCnt_++;
   ioThreadWakeup();
   pthread_mutex_unlock(&reqMutex_);
}

// Set run command request
void CommLink::setRunCommand ( uint32_t linkConfig, Command *cmd ) {
  cout<<"[CommLink:dev] setRunCommand ==> "<<cmd<<endl;
   pthread_mutex_lock(&reqMutex_);
   runReqEntry_ = cmd;
   runReqConf_  = linkConfig;
   pthread_mutex_unlock(&reqMutex_);
}

// Get data count
uint32_t CommLink::dataFileCount () {
   return(dataFileCount_);
}

// Get data receive count
uint32_t CommLink::dataRxCount() {
   return(dataRxCount_);
}

// Get register rx count
uint32_t CommLink::regRxCount() {
   return(regRxCount_);
}

// Get timeout count
uint32_t CommLink::timeoutCount() {
   return(timeoutCount_);
}

// Get error count
uint32_t CommLink::errorCount() {
   return(errorCount_);
}

// Get unexpcted count
uint32_t CommLink::unexpectedCount() {
   return(unexpCount_);
}

// Clear counters
void CommLink::clearCounters() {
   dataFileCount_ = 0;
   dataRxCount_   = 0;
   regRxCount_    = 0;
   timeoutCount_  = 0;
   errorCount_    = 0;
   unexpCount_    = 0;
}


// Set data configuration for data reception
void CommLink::setDataMask ( uint32_t mask ) {
   dataSource_ = mask;
}

// Add data source
void CommLink::addDataSource (uint32_t source ) {
   dataSource_ = source;
}

// Set max rx size
void CommLink::setMaxRxTx (uint32_t size) {
   stringstream err;

   // Start io thread
   if ( runEnable_ ) {
      err << "CommLink::setMaxRxTx -> Cannot set maxRxTx while open" << endl;
      if ( debug_ ) cout << err.str();
      throw(err.str());
   }

   maxRxTx_ = size;

}

// Add configuration to data file
void CommLink::addConfig ( string config ) {
   uint32_t currResp;
   string err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   xmlReqEntry_ = config;
   xmlType_     = Data::XmlConfig;
   currResp     = xmlRespCnt_;
   xmlReqCnt_++;
   initTime(&tm);
   dataThreadWakeup();

   // Wait for response, 1 second
   while ( currResp == xmlRespCnt_ ) {
      if ( timePassed(&tm,1000000) ) {
         initTime(&tm);
         cout << "CommLink::addConfig -> Waiting for main thread!" << endl;
      }
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}

// Add status to data file
void CommLink::addStatus ( string status ) {
   uint32_t  currResp;
   string err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   xmlReqEntry_ = status;
   xmlType_     = Data::XmlStatus;
   currResp     = xmlRespCnt_;
   xmlReqCnt_++;
   initTime(&tm);
   dataThreadWakeup();

   // Wait for response, 1 second
   while ( currResp == xmlRespCnt_ ) {
      if ( timePassed(&tm,1000000) ) {
         initTime(&tm);
         cout << "CommLink::addStatus -> Waiting for main thread!" << endl;
      }
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}

// Add run start to data file
void CommLink::addRunStart ( string xml ) {
  uint32_t currResp;
   string err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   xmlReqEntry_ = xml;
   xmlType_     = Data::XmlRunStart;
   currResp     = xmlRespCnt_;
   xmlReqCnt_++;
   initTime(&tm);
   dataThreadWakeup();

   // Wait for response, 1 second
   while ( currResp == xmlRespCnt_ ) {
      if ( timePassed(&tm,1000000) ) {
         initTime(&tm);
         cout << "CommLink::addRunStart -> Waiting for main thread!" << endl;
      }
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}

// Add status to data file
void CommLink::addRunStop ( string xml ) {
   uint32_t currResp;
   string err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   xmlReqEntry_ = xml;
   xmlType_     = Data::XmlRunStop;
   currResp     = xmlRespCnt_;
   xmlReqCnt_++;
   initTime(&tm);
   dataThreadWakeup();

   // Wait for response, 1 second
   while ( currResp == xmlRespCnt_ ) {
      if ( timePassed(&tm,1000000) ) {
         initTime(&tm);
         cout << "CommLink::addRunStop -> Waiting for main thread!" << endl;
      }
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}

// Add timestamp to data file
void CommLink::addRunTime ( string xml ) {
   uint32_t currResp;
   string err;
   struct timeval tm;

   pthread_mutex_lock(&reqMutex_);

   // Setup request
   xmlReqEntry_ = xml;
   xmlType_     = Data::XmlRunTime;
   currResp     = xmlRespCnt_;
   xmlReqCnt_++;
   initTime(&tm);
   dataThreadWakeup();

   // Wait for response, 1 second
   while ( currResp == xmlRespCnt_ ) {
      if ( timePassed(&tm,1000000) ) {
         initTime(&tm);
         cout << "CommLink::addRunTime -> Waiting for main thread!" << endl;
      }
      mainThreadWait(100);
   }
   pthread_mutex_unlock(&reqMutex_);
}


// Enable store of config/status/start/stop to data file & callback
void CommLink::setXmlStore ( bool enable ) {
   xmlStoreEn_ = enable;
}

// Enable shared 
void CommLink::enableSharedMemory ( string system, uint32_t id ) {

   // Attempt to open and init shared memory
   if ( (smemFd_ = dataSharedOpenAndMap ( (DataSharedMemory **)(&smem_) , system.c_str(), id )) < 0 ) {
      smem_ = NULL;
      throw string("CommLink::enabledSharedMemory -> Failed to open shared memory");
   }

   // Init shared memory
   dataSharedInit((DataSharedMemory*)smem_);
}

// Init timer
void CommLink::initTime(struct timeval *tm) {
   gettimeofday(tm,NULL);
}

// Check timer
uint32_t CommLink::timePassed(struct timeval *tm, uint32_t usec) {
   struct timeval endTime;
   struct timeval sumTime;
   struct timeval nowTime;

   sumTime.tv_sec = (usec / 1000000);
   sumTime.tv_usec = (usec % 1000000);

   timeradd(tm,&sumTime,&endTime);

   gettimeofday(&nowTime,NULL);

   if ( timercmp(&nowTime,&endTime,>) ) {
      printf("CommLink::timePassed -> Detected timeout. usec = %i, start_sec = %i, start_usec = %i, "
             "end_sec = %i, end_usec = %i, now_sec = %i, now_usec = %i, ", 
             usec, (uint32_t)tm->tv_sec, (uint32_t)tm->tv_usec, 
             (uint32_t)endTime.tv_sec, (uint32_t)endTime.tv_usec, (uint32_t)nowTime.tv_sec, (uint32_t)nowTime.tv_usec );
      return(1);
   }
   else return(0);
}

// Wait in data thread
void CommLink::dataThreadWait(uint32_t usec) {
   struct timespec timeout;

   pthread_mutex_lock(&dataMutex_);

   clock_gettime(CLOCK_REALTIME,&timeout);

   // Avoid costly divides if possible
   if ( usec >= 1000*1000) {
      timeout.tv_sec  += usec / 1000000;
      timeout.tv_nsec += (usec % 1000000) * 1000;
   } else {
      timeout.tv_nsec += usec * 1000;
   }

   if ( timeout.tv_nsec > (1000 * 1000 * 1000) ) {
     timeout.tv_nsec -= (1000 * 1000 * 1000);
     timeout.tv_sec  += 1;
   } 

   pthread_cond_timedwait(&dataCondition_, &dataMutex_, &timeout);
   pthread_mutex_unlock(&dataMutex_);
}

// Wakeup data thread
void CommLink::dataThreadWakeup() {
   pthread_cond_signal(&dataCondition_);
}

// Wait in io thread
void CommLink::ioThreadWait(uint32_t usec) {
   struct timespec timeout;

   pthread_mutex_lock(&ioMutex_);

   clock_gettime(CLOCK_REALTIME,&timeout);

   // Avoid costly divides if possible
   if ( usec >= 1000*1000) {
      timeout.tv_sec  += usec / 1000000;
      timeout.tv_nsec += (usec % 1000000) * 1000;
   } else {
      timeout.tv_nsec += usec * 1000;
   }

   if ( timeout.tv_nsec > (1000 * 1000 * 1000) ) {
     timeout.tv_nsec -= (1000 * 1000 * 1000);
     timeout.tv_sec  += 1;
   } 

   pthread_cond_timedwait(&ioCondition_, &ioMutex_, &timeout);
   pthread_mutex_unlock(&ioMutex_);
}

// Wakeup io thread
void CommLink::ioThreadWakeup() {
   pthread_cond_signal(&ioCondition_);
}

// Wake in main thread
void CommLink::mainThreadWait(uint32_t usec) {
   struct timespec timeout;

   pthread_mutex_lock(&mainMutex_);

   clock_gettime(CLOCK_REALTIME,&timeout);

   // Avoid costly divides if possible
   if ( usec >= 1000*1000) {
      timeout.tv_sec  += usec / 1000000;
      timeout.tv_nsec += (usec % 1000000) * 1000;
   } else {
      timeout.tv_nsec += usec * 1000;
   }

   if ( timeout.tv_nsec > (1000 * 1000 * 1000) ) {
     timeout.tv_nsec -= (1000 * 1000 * 1000);
     timeout.tv_sec  += 1;
   } 

   pthread_cond_timedwait(&mainCondition_, &mainMutex_, &timeout);
   pthread_mutex_unlock(&mainMutex_);
}

// Wakeup main thread
void CommLink::mainThreadWakeup() {
   pthread_cond_signal(&mainCondition_);
}

