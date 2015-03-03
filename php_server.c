/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2014 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_php_server.h"

/* 包含需要用到的头文件 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <malloc.h>

/* If you declare any globals in php_php_server.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(php_server)
*/

/* True global resources - no need for thread safety here */
static int le_php_server;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
PHP_INI_END()
*/
/* }}} */



/* 定义一些常亮 */
//最大的子进程数量
#define MAX_PROCESS_NUMBER 16

//每个子进程最多能够处理的客户连接数量
#define MAX_USER_CONNECT 65536

//epoll最多能够的处理的事件的数量
#define MAX_EPOLL_NUMBER 10000

//调试的宏
#define PHP_SERVER_DEBUG printf

/* 一些常用的函数  */

/* 设置文件描述符为非阻塞  */
int php_server_set_nonblock(int fd){
	int old_option,new_option;
	old_option = fcntl(fd,F_GETFL);
	new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}

/* 为某个fd向epoll事件表添加边沿触发式可读事件  */
void php_server_epoll_add_read_fd(int epoll_fd,int fd){
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd, &event);
	php_server_set_nonblock(fd);
}

/* 把某个fd移除出epoll事件表*/
void php_server_epoll_del_fd(int epoll_fd,int fd){
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL,fd,0);
	close(fd);
}


/* 把fd从epool事件表中移除 */
void php_server_epoll_remove_fd(int epoll_fd,int fd){
	epoll_ctl(epoll_fd,EPOLL_CTL_DEL,fd,0);
	close(fd);
}


/* 定义存储进程信息的结构体 */
typedef struct php_server_process{
	//当前子进程的数量
	unsigned int	process_number;
		
	//当前进程的序号,父进程为-1,子进程从0开始
	int		process_index;

	//子进程的PID
	pid_t * child_pid;

	//epoll事件表标识
	int epoll_fd;

	//整个应用监听的sokect
	int socket_fd;

	//当前进程是否停止运行
	zend_bool is_stop;

	//用作进程间通讯的双端管道
	int ** pipe_fd;	
}server_process;

server_process *  process_global;


/* 监听的socket_fd  */
int socket_fd_global;

zend_bool php_server_setup_socket(char * ip,int port){
	
	int ret;
	struct sockaddr_in address;
	
	socket_fd_global = socket(PF_INET,SOCK_STREAM,0);
	if(socket_fd_global<0){
		return FAILURE;
	}

	bzero(&address,sizeof(address));

	address.sin_family = AF_INET;

	inet_pton(AF_INET,ip,&address.sin_addr);

	address.sin_port = htons(port);

	ret = bind(socket_fd_global,(struct sockaddr * )&address,sizeof(address));

	if(ret == -1){
		return FAILURE;
	}
	
	ret = listen(socket_fd_global,5);

	if(ret == -1){
		return FAILURE;
	}

	return SUCCESS;
}

/* 关闭监听的socket */

zend_bool php_server_shutdown_socket(){
	close(socket_fd_global);
	return SUCCESS;
}

/*创建进程池*/

zend_bool php_server_setup_process_pool(int socket_fd,	unsigned int process_number){
	
	int i,ret;
	pid_t pid;

	process_global = (server_process * ) malloc(sizeof(server_process));

	process_global->pipe_fd = (int **)malloc(sizeof(int * ) * process_number);

	process_global->socket_fd = socket_fd;

	process_global->process_number = 0;
	
	process_global->is_stop = 1;	

	for(i=0;	i<process_number;	i++){

		process_global->socket_fd = socket_fd_global;
		
		process_global->pipe_fd[i] = (int *) malloc(sizeof(int) * 2);		

		ret = socketpair(PF_UNIX,	SOCK_STREAM,	0,process_global->pipe_fd[i]);

		if(ret!=0){
			return FAILURE;
		}

		pid = fork();

		if(pid > 0){
			PHP_SERVER_DEBUG("child %d is created\n",pid);
			if(0 == i){
				process_global->child_pid = (pid_t * ) malloc(sizeof(pid_t) * process_number);
				process_global->process_number = process_number;
			}
			process_global->process_index = -1;
			close(process_global->pipe_fd[i][1]);
			continue;
		}else if(0 == pid){
			process_global->process_index = i;
			close(process_global->pipe_fd[i][0]);
			break;
		}else{
			return FAILURE;
		}
	}

	return SUCCESS;
}
/* 信号处理函数  */
void php_server_sig_handler(int signal_no){
	
	static int killed_child = 0;
	int i;
	pid_t pid;	

	PHP_SERVER_DEBUG("server %d get %d(%d:%d:%d)\n",process_global->process_index,signal_no,SIGCHLD,SIGTERM,SIGINT);

	if(process_global->process_index == -1){
		switch(signal_no){
			/* 如果子进程退出，那么设置其pid为-1并关闭其管道  */
			case SIGCHLD:
			if((pid = waitpid(-1,NULL,WNOHANG)) > 0){
				for(i=0; i<process_global->process_number; i++){
					if(pid == process_global->child_pid[i]){
						process_global->child_pid[i] = -1;
						close(process_global->pipe_fd[i][0]);
						PHP_SERVER_DEBUG("child %d:%d out\n",pid,i);
						killed_child++;
						break;
					}
				}
			}
			/* 当退出的进程数量达到子进程的总量，那么父进程也紧跟着退出 */
			if(killed_child == process_global->process_number){
				process_global->is_stop = 1;
			}
			PHP_SERVER_DEBUG("pid %d get,killed child %d\n",pid,killed_child);
			break;

			case SIGTERM:
			case SIGINT:
			/* 向所有还存活的进程发送信号 */
			for(i=0;i<process_global->process_number;i++){
				if(-1 != process_global->child_pid[i]){
					kill(process_global->child_pid[i],SIGTERM);
				}
			}
			break;
		}
	}else{
		switch(signal_no){
			case SIGTERM:
			case SIGINT:
			process_global->is_stop = 1;
			break;
		}
	}
}

/* 启动初始化函数 */
zend_bool php_server_run_init(){
	process_global->epoll_fd = epoll_create(5);
	if(process_global->epoll_fd == -1){
		return FAILURE;
	}
	process_global->is_stop = 0;
	/*设置信号处理函数*/
	signal(SIGCHLD,php_server_sig_handler);
	signal(SIGTERM,php_server_sig_handler);
	signal(SIGINT,php_server_sig_handler);
	return SUCCESS;
}

/* 启动函数的清理函数 */
zend_bool php_server_clear_init(){

	close(process_global->epoll_fd);
	return SUCCESS;
}

/* 启动父进程*/
int php_server_run_master_process(){
		
	struct epoll_event events[MAX_EPOLL_NUMBER];
	int i,number,sock_fd,child_index = 0,child_pid,child_pipe,alive_child;
	char data = '1';

	php_server_run_init();
	
	/* 父进程监听global_socket */
	php_server_epoll_add_read_fd(process_global->epoll_fd, process_global->socket_fd);

	PHP_SERVER_DEBUG("master while\n");
	
	while(!process_global->is_stop){
		number = epoll_wait(process_global->epoll_fd,events,MAX_EPOLL_NUMBER,-1);
		PHP_SERVER_DEBUG("epoll come in master:%d\n",number);
		if(number<0 && errno != EINTR){
			break; 
		}
		
		for(i=0;i<number;i++){
			sock_fd = events[i].data.fd;
				/*说明有的新的连接到来，那么轮询一个进程处理这个连接*/
			if(sock_fd == process_global->socket_fd){
				alive_child = process_global->process_number;
				while(-1 == process_global->child_pid[child_index]){
					child_index = (child_index+1)%process_global->process_number;
					alive_child--;
					if(0 == alive_child){
						break;
					}
				}
				/* 如果子进程存活量为0了，那么就kill自己,然后父进程会通知所有存在的子进程杀掉自己*/
				if(alive_child == 0){
					PHP_SERVER_DEBUG("child none, master start kill itself\n");
					kill(getpid(),SIGTERM);
					break;
				}
				child_pid = process_global->child_pid[child_index];
				child_pipe = process_global->pipe_fd[child_index][0];
				send(child_pipe,(char *) & data,sizeof(data) , 0);
				child_index = (child_index+1)%process_global->process_number;
			}
		}
	}
	
	php_server_clear_init();
	if(number<0){
		return errno;
	}
	return 0;
}

/* 子进程循环读取消息 */
#define BUFFER_SIZE 8096
char recv_buffer[BUFFER_SIZE];

char * php_server_recv_from_client(int sock_fd){
	int ret;

	bzero(recv_buffer,sizeof(recv_buffer));	

	ret = recv(sock_fd,recv_buffer,sizeof(recv_buffer),0);

	/* 说明读操作错误，那么不再监听这个连接 */
	if(ret<0){
		if(errno != EAGAIN){
			php_server_epoll_del_fd(process_global->epoll_fd,sock_fd);	
		}
		PHP_SERVER_DEBUG("worker recv error\n");
		
		return NULL;
	}else if(ret == 0){
		/* 说明客户端关闭了连接 */
		php_server_epoll_del_fd(process_global->epoll_fd,sock_fd);
		PHP_SERVER_DEBUG("client %d closed\n",sock_fd);

		return NULL;
	}else{
		PHP_SERVER_DEBUG("%d recv from %d:%s\n",process_global->process_index,sock_fd,recv_buffer);

		return recv_buffer;
	}
}



/* 启动子进程  */
int php_server_run_worker_process(){
	
	struct epoll_event events[MAX_EPOLL_NUMBER];
	int i,number,ret,sock_fd,conn_fd,parent_pipe_fd = process_global->pipe_fd[process_global->process_index][1];
	char data = '0';
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);
	php_server_run_init();
	
	php_server_epoll_add_read_fd(process_global->epoll_fd,parent_pipe_fd);
	/* 添加父管道的可读事件，以便知道有新的连接到来了 */
	while(!process_global->is_stop){
		number = epoll_wait(process_global->epoll_fd,events,MAX_EPOLL_NUMBER,-1);
		if(number<0 && errno != EINTR){
			return	errno; 
		}
		
		for(i=0;i<number;i++){
			sock_fd = events[i].data.fd;
			if(sock_fd == parent_pipe_fd && (events[i].events & EPOLLIN)){
				ret = recv(sock_fd,(char *) & data, sizeof(data),0);
				if( ( ret < 0 && errno != EAGAIN) || ret == 0 ){
					continue;
				}else{
					//有新的连接到来
					bzero(&client,client_len);
					conn_fd = accept(process_global->socket_fd,(struct sockaddr *) & client, &client_len);
					
					PHP_SERVER_DEBUG("worker %d accepted\n",process_global->process_index);
					PHP_SERVER_DEBUG("accept sockfd in worker:%d\n",conn_fd);			
					if(conn_fd < 0){
						continue;
					}
					php_server_epoll_add_read_fd(process_global->epoll_fd,conn_fd);
				}
			/*这里除开父进程发送的消息，就只有客户端的消息到来了*/
			}else if(events[i].events & EPOLLIN){
				
				php_server_recv_from_client(sock_fd);
			}
		}	
	}

	php_server_clear_init();
	return 0;
}

/* 父子进程统一启动进程的函数*/
void php_server_run(){
	if(process_global->process_index == -1){
		php_server_run_master_process();
	}else{
		php_server_run_worker_process();
	}
}


/* 销毁进程池  */
zend_bool php_server_shutdown_process_pool(unsigned int process_number){
	int i;
	for(i = 0; i<process_number; i++){
		free(process_global->pipe_fd[i]);
	}
	free(process_global->pipe_fd);
	if(process_global->process_index == -1){
		free(process_global->child_pid);
	}
	free(process_global);
	return SUCCESS;
}



/* {{{ 测试模块是否正常加载
 */
PHP_FUNCTION(test_php_server){
	int ret,process_number=7;

	PHP_SERVER_DEBUG("function is ok !\n");

	ret = php_server_setup_socket("127.0.0.1",9000);
	PHP_SERVER_DEBUG("setup_socket:%d\n",ret);

	ret = php_server_setup_process_pool(socket_fd_global,process_number);
	PHP_SERVER_DEBUG("setup_process_pool:%d\n",ret);

	PHP_SERVER_DEBUG("server %d is running.\n",process_global->process_index);
	php_server_run();
	PHP_SERVER_DEBUG("server %d is stopping.\n",process_global->process_index);

	ret = php_server_shutdown_process_pool(process_number);
	PHP_SERVER_DEBUG("server %d shutdown_process_pool:%d\n",process_global->process_index,ret);

	ret = php_server_shutdown_socket();
	PHP_SERVER_DEBUG("server %d shutdown_socket:%d\n",process_global->process_index,ret);	
			
	RETURN_STRING("php-server");
}
/* }}} */




/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(php_server)
{
	/* If you have INI entries, uncomment these lines
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(php_server)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(php_server)
{
#if defined(COMPILE_DL_PHP_SERVER) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE;
#endif
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(php_server)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(php_server)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "php_server support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ php_server_functions[]
 *
 * Every user visible function must have an entry in php_server_functions[].
 */
const zend_function_entry php_server_functions[] = {
	PHP_FE(test_php_server , NULL)
	PHP_FE_END	/* Must be the last line in php_server_functions[] */
};
/* }}} */

/* {{{ php_server_module_entry
 */
zend_module_entry php_server_module_entry = {
	STANDARD_MODULE_HEADER,
	"php_server",
	php_server_functions,
	PHP_MINIT(php_server),
	PHP_MSHUTDOWN(php_server),
	PHP_RINIT(php_server),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(php_server),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(php_server),
	PHP_PHP_SERVER_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PHP_SERVER
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE;
#endif
ZEND_GET_MODULE(php_server)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */