/*
 * Copyright 2007 Stephen Liu
 * For license terms, see the file COPYING along with this library.
 */

#include <string.h>
#include <assert.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "spproclfsvr.hpp"

#include "spprocinet.hpp"
#include "spprocmanager.hpp"
#include "spprocpool.hpp"
#include "spprocpdu.hpp"

class SP_ProcWorkerLFAdapter : public SP_ProcWorker {
public:
	SP_ProcWorkerLFAdapter( int listenfd, SP_ProcInetServiceFactory * factory );
	~SP_ProcWorkerLFAdapter();

	void setMaxRequestsPerProc( int maxRequestsPerProc );

	virtual void process( SP_ProcInfo * procInfo );

private:
	int mListenfd;
	SP_ProcInetServiceFactory * mFactory;

	int mMaxRequestsPerProc;
};

SP_ProcWorkerLFAdapter :: SP_ProcWorkerLFAdapter( int listenfd, SP_ProcInetServiceFactory * factory )
{
	mListenfd = listenfd;
	mFactory = factory;

	mMaxRequestsPerProc = 0;
}

SP_ProcWorkerLFAdapter :: ~SP_ProcWorkerLFAdapter()
{
	delete mFactory;
	mFactory = NULL;
}

void SP_ProcWorkerLFAdapter :: setMaxRequestsPerProc( int maxRequestsPerProc )
{
	mMaxRequestsPerProc = maxRequestsPerProc;
}

void SP_ProcWorkerLFAdapter :: process( SP_ProcInfo * procInfo )
{
	mFactory->workerInit( procInfo );

	for( ; ( 0 == mMaxRequestsPerProc )
			|| ( mMaxRequestsPerProc > 0 && procInfo->getRequests() <= mMaxRequestsPerProc ); ) {

		struct sockaddr_in clientAddr;
		socklen_t clientLen = sizeof( clientAddr );
		int fd = accept( mListenfd, (struct sockaddr *)&clientAddr, &clientLen );
		if( fd >= 0 ) {
			assert( write( procInfo->getPipeFd(), &SP_ProcLFServer::CHAR_BUSY, 1 ) > 0 );

			SP_ProcInetService * service = mFactory->create();
			service->handle( fd );
			close( fd );
			delete service;

			assert( write( procInfo->getPipeFd(), &SP_ProcLFServer::CHAR_IDLE, 1 ) > 0 );

			procInfo->setRequests( procInfo->getRequests() + 1 );
		} else {
			syslog( LOG_WARNING, "WARN: accept fail, errno %d, %s", errno, strerror( errno ) );

			if( errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EPROTO || errno == EINTR ) {
				// ignore these errno
			} else {
				assert( write( procInfo->getPipeFd(), &SP_ProcLFServer::CHAR_EXIT, 1 ) > 0 );
				break;
			}
		}

		char pod = 0;
		if( recv( procInfo->getPipeFd(), &pod, 1, MSG_DONTWAIT ) > 0 ) {
			assert( write( procInfo->getPipeFd(), &SP_ProcLFServer::CHAR_EXIT, 1 ) > 0 );
			break;
		}
	}

	procInfo->setLastActiveTime( time( NULL ) );

	mFactory->workerEnd( procInfo );
}

//-------------------------------------------------------------------

class SP_ProcWorkerFactoryLFAdapter : public SP_ProcWorkerFactory {
public:
	SP_ProcWorkerFactoryLFAdapter( int listenfd, SP_ProcInetServiceFactory * factory );
	virtual ~SP_ProcWorkerFactoryLFAdapter();

	void setMaxRequestsPerProc( int maxRequestsPerProc );

	virtual SP_ProcWorker * create() const;

private:
	int mListenfd;
	SP_ProcInetServiceFactory * mFactory;

	int mMaxRequestsPerProc;
};

SP_ProcWorkerFactoryLFAdapter :: SP_ProcWorkerFactoryLFAdapter(
		int listenfd, SP_ProcInetServiceFactory * factory )
{
	mListenfd = listenfd;
	mFactory = factory;

	mMaxRequestsPerProc = 0;
}

SP_ProcWorkerFactoryLFAdapter :: ~SP_ProcWorkerFactoryLFAdapter()
{
	delete mFactory;
	mFactory = NULL;
}

void SP_ProcWorkerFactoryLFAdapter :: setMaxRequestsPerProc( int maxRequestsPerProc )
{
	mMaxRequestsPerProc = maxRequestsPerProc;
}

SP_ProcWorker * SP_ProcWorkerFactoryLFAdapter :: create() const
{
	SP_ProcWorkerLFAdapter * worker = new SP_ProcWorkerLFAdapter( mListenfd, mFactory );
	worker->setMaxRequestsPerProc( mMaxRequestsPerProc );

	return worker;
}

//-------------------------------------------------------------------

const char SP_ProcLFServer :: CHAR_BUSY = 'B';
const char SP_ProcLFServer :: CHAR_IDLE = 'I';
const char SP_ProcLFServer :: CHAR_EXIT = '!';

SP_ProcLFServer :: SP_ProcLFServer( const char * bindIP, int port,
		SP_ProcInetServiceFactory * factory )
{
	strncpy( mBindIP, bindIP, sizeof( mBindIP ) );
	mBindIP[ sizeof( mBindIP ) - 1 ] = '\0';

	mPort = port;

	mFactory = factory;

	mMaxProc = 64;
	mMaxIdleProc = 5;
	mMinIdleProc = 1;

	mMaxRequestsPerProc = 0;

	mIsStop = 1;
}

SP_ProcLFServer :: ~SP_ProcLFServer()
{
}

void SP_ProcLFServer :: setMaxProc( int maxProc )
{
	mMaxProc = maxProc;
}

void SP_ProcLFServer :: setMaxRequestsPerProc( int maxRequestsPerProc )
{
	mMaxRequestsPerProc = maxRequestsPerProc;
}

void SP_ProcLFServer :: setMaxIdleProc( int maxIdleProc )
{
	mMaxIdleProc = maxIdleProc;
}

void SP_ProcLFServer :: setMinIdleProc( int minIdleProc )
{
	mMinIdleProc = minIdleProc;
}

int SP_ProcLFServer :: isStop()
{
	return mIsStop;
}

void SP_ProcLFServer :: shutdown()
{
	mIsStop = 1;
}

int SP_ProcLFServer :: start()
{
	/* Don't die with SIGPIPE on remote read shutdown. That's dumb. */
	signal( SIGPIPE, SIG_IGN );
	signal( SIGCHLD, SIG_IGN );

	int listenfd = -1;
	assert( 0 == SP_ProcPduUtils::tcp_listen( mBindIP, mPort, &listenfd ) );

	SP_ProcWorkerFactoryLFAdapter * factory =
			new SP_ProcWorkerFactoryLFAdapter( listenfd, mFactory );
	factory->setMaxRequestsPerProc( mMaxRequestsPerProc );

	SP_ProcManager procManager( factory );
	procManager.start();
	SP_ProcPool * procPool = procManager.getProcPool();

	close( listenfd );

	if( mMinIdleProc <= 0 ) mMinIdleProc = 1;
	if( mMaxIdleProc < mMinIdleProc ) mMaxIdleProc = mMinIdleProc;
	if( mMaxProc <= 0 ) mMaxProc = mMaxIdleProc;

	SP_ProcInfoList procList;

	for( int i = 0; i < mMinIdleProc; i++ ) {
		SP_ProcInfo * info = procPool->get();
		if( NULL != info ) {
			procList.append( info );
		} else {
			syslog( LOG_WARNING, "WARN: Create proc fail, only %d idle proc",
					procList.getCount() );
			break;
		}
	}

	int idleCount = procList.getCount();

	mIsStop = 0;

	for( ; 0 == mIsStop ; ) {
		fd_set rset;
		FD_ZERO( &rset );

		int maxfd = 0;

		for( int i = 0; i < procList.getCount(); i++ ) {
			const SP_ProcInfo * iter = procList.getItem( i );
			FD_SET( iter->getPipeFd(), &rset );
			maxfd = maxfd > iter->getPipeFd() ? maxfd : iter->getPipeFd();
		}

		int nsel = select( maxfd + 1, &rset, NULL, NULL, NULL );

		/* find out the child is busy/idle/exit */
		for( int i = procList.getCount() - 1; i >= 0; i-- ) {
			SP_ProcInfo * iter = procList.getItem( i );

			if( FD_ISSET( iter->getPipeFd(), &rset ) ) {
				int isProcExit = 0;

				char buff[ 128 ] = { 0 };
				int len = recv( iter->getPipeFd(), buff, sizeof( buff ), MSG_DONTWAIT );
				if( len > 0 ) {
					iter->setRequests( iter->getRequests() + len );
					if( CHAR_IDLE == buff[ len - 1 ] ) {
						if( ! iter->isIdle() ) idleCount++;
						iter->setIdle( 1 );
					} else if( CHAR_BUSY == buff[ len - 1 ] ) {
						if( iter->isIdle() ) idleCount--;
						iter->setIdle( 0 );
					} else {
						isProcExit = 1;
					}
				} else if( 0 == len ) {
					isProcExit = 1;
				}

				if( isProcExit ) {
					syslog( LOG_INFO, "INFO: proc #%u exit", iter->getPid() );
					if( iter->isIdle() ) idleCount--;
					iter = procList.takeItem( i );
					procPool->erase( iter );
				}
				if (--nsel == 0) break;	/* all done with select() results */
			}
		}

		if( idleCount > mMaxIdleProc ) {
			for( int i = 0; i < procList.getCount(); i++ ) {
				SP_ProcInfo * iter = procList.getItem( i );
				if( iter->isIdle() ) {
					syslog( LOG_INFO, "INFO: idle.count %d, max.idle %d, force proc #%u to exit",
						idleCount, mMaxIdleProc, iter->getPid() );
					write( iter->getPipeFd(), &CHAR_EXIT, 1 );
					iter->setIdle( 0 );
					idleCount--;

					break;
				}
			}
		}

		if( idleCount < mMinIdleProc && procList.getCount() < mMaxProc ) {
			SP_ProcInfo * info = procPool->get();
			if( NULL != info ) {
				idleCount++;
				procList.append( info );
			} else {
				syslog( LOG_WARNING, "WARN: Create proc fail, only %d idle proc", idleCount );
				break;
			}
		}
	}

	return 0;
}

