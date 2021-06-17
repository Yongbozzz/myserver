#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "./timer/lst_timer.h"
//#include "./log/log.h"
#include "./database/sql_connection_pool.h"
#define MAX_FD 655
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5             //最小超时单位
#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞


extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );
extern int setnonblocking(int fd);
//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;


//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    //LOG_INFO("close fd %d", user_data->sockfd);
    //Log::get_instance()->flush();
}
//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


int main( int argc, char* argv[] )
{

#ifdef ASYNLOG
    Log::get_instance()->init("./ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("./ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    addsig( SIGPIPE, SIG_IGN );
    //创建数据库连接池
    connection_pool *m_connPool;
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", "root", "root", "yourdb", 3306, 8,0);
    
    //connection_pool *connPool = connection_pool::GetInstance();
    //connPool->init("localhost", "jojo", "root", "mytestdb", 3306, 8);
    http_conn* users = new http_conn[ MAX_FD ];
    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >;
    }
    catch( ... )
    {
        return 1;
    }


    assert( users );

    users->initmysql_result(m_connPool);
    int user_count = 0;

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );


    //struct linger tmp = { 1, 0 };
    //setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    //address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons( port );
//add at 2021/6/14
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);
    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if (( number < 0 ) && ( errno != EINTR ) )
        {
            //printf( "epoll failure\n" );
	    //LOG_ERROR("%s", "epoll failure");
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
#ifdef listenfdLT
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    //printf( "errno is: %d\n", errno );
			//LOG_ERROR( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
		    //LOG_ERROR("%s", "Internal server busy");                    
		    continue;
                }
               	
		char server_path[200];
    		getcwd(server_path, 200);
    		char root[6] = "/root";
    		char *m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    		strcpy(m_root, server_path);
    		strcat(m_root, root);

		
                users[connfd].init( connfd, client_address,"/home/jojo/myserver/mywork/root","root","root","yourdb");
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif
#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        //LOG_ERROR("%s:errno is:%d", "accept error", errno);
			
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        //LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                //users[sockfd].close_conn();
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
//处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
//处理客户连接上接收到的数据
            else if( events[i].events & EPOLLIN )
            {
		util_timer *timer = users_timer[sockfd].timer;
                if( users[sockfd].read() )
                {
                    pool->append( users + sockfd );
                   //若有数据传输，则将定时器往后延迟3个单位
                   //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        //LOG_INFO("%s", "adjust timer once");
                        //Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if( events[i].events & EPOLLOUT )
            {
		util_timer *timer = users_timer[sockfd].timer;
                if( !users[sockfd].write() )
                {
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        //LOG_INFO("%s", "adjust timer once");
                        //Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
		   
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
	}
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
        
    }

    close( epollfd );
    close( listenfd );
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;
    delete[] users_timer;
    return 0;
}
