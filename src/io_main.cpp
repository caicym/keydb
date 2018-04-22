#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#if 1
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <event.h>
#include <pthread.h>
#include "evutil.h"
#include "easy_log.h"
#include "io_main.h"
#include <fstream> 
#include "json/json.h"
#include <string>

//////////////global structure///////////////////////////
/////

pthread_t ptocess_thread[MAX_PROCESS_THREAD] = {0};
pthread_t update_ptocess_thread[UPDATE_PROCESS_THREAD] = {0};

struct sig_ev_arg notify_arg[MAX_PROCESS_THREAD] = {0};
struct sig_ev_arg update_notify_arg[UPDATE_PROCESS_THREAD] = {0};

easy_log g_log("./log/comse.log",(4000000),8);
/////////////////////////////////////////////////////////

int g_recv_buff = (1024 * 4);
int g_send_buff = (1024 * 4);

short g_search_port = 8807;
short g_update_port = 8907;
char g_search_ip[64] = "0.0.0.0";
char g_update_ip[64] = "0.0.0.0";
int g_listen_backlog = -1;

//timeout
struct timeval tv_con_timeout = {0};
struct timeval tv_recv_timeout = {0,100000};
struct timeval tv_send_timeout = {0,1000};//not use,send event occur very fast.Use it occur many send timeout.


//search thread num
int g_search_thread_num = 3;
int g_now_max_thread_id = -1;

//accept lock
pthread_mutex_t g_search_port_lock = PTHREAD_MUTEX_INITIALIZER;

//keydb need
std::string g_index_parent_file = "";
std::string g_key_load_file = "";
std::string g_key_dump_file = "";

/////////////////////////////////////////////////////////////

struct interface_data * it_pool_get()
{
	return new interface_data(g_recv_buff,g_send_buff);
}

void it_pool_put(struct interface_data *p_it)
{
	if (p_it) {delete p_it;}
}

void inline reg_add_event(struct event *p_eve,int fd,int event_type,event_cb_func p_func,void *p_arg,
		struct event_base *p_base,struct timeval *p_time)
{
	if (0 == p_eve
			|| 0 == p_func
			|| 0 == p_base)
		return;
	event_set(p_eve,fd,event_type,p_func,p_arg);
	event_base_set(p_base,p_eve);
	// add recv timeout
	event_add(p_eve,p_time);
}
//////////////callback function/////////////////
void SendData(int fd, short int events, void *arg)
{
    if ( 0 == arg || fd <= 0)
        return;
    struct interface_data *p_it = (struct interface_data *)arg;
    struct event *p_ev = &(p_it->ev);
    struct event_base *base = p_ev->ev_base;
    char *buff = 0;
    int buff_len = 0;
    int len;
    char log_buff[BUFF_SIZE] = {0};

    if (events == EV_TIMEOUT)
    {
        close(p_ev->ev_fd);
        event_del(p_ev);
        it_pool_put(p_it);
        snprintf(log_buff,sizeof(log_buff),"send timeout fd=[%d] error[%d]", fd, errno);
        g_log.write_record(log_buff);
        return;
    } 

    int again_count = 0;
    while(1)
    {
        buff = (char *)(p_it->send_buff) + p_it->before_sended_len;
        buff_len = p_it->now_send_len - p_it->before_sended_len;
        len = send(fd, buff, buff_len, MSG_NOSIGNAL);
        if (len >= 0)
        {
            p_it->before_sended_len += len;
        }

        if (p_it->before_sended_len >= p_it->now_send_len)
        {break;}

        if (errno != EAGAIN &&
                errno != 0)
        {break;}

        if (++again_count >= 5)
        {
            snprintf(log_buff,sizeof(log_buff),"Thread[%d]\tClient[%d]\tresend_break",
                    p_it->thread_id, fd);
            g_log.write_record(log_buff);
            break;
        }
        else
        {
            snprintf(log_buff,sizeof(log_buff),"Thread[%d]\tClient[%d]\tresend[%d]\t"
                    "data[before_sended_len=%d,now_send_len=%d]",
                    p_it->thread_id, fd, 
                    again_count,p_it->before_sended_len, p_it->now_send_len);
            g_log.write_record(log_buff);
            usleep(100);
        }
    }

    if (p_it->now_send_len < p_it->send_buff_len) {p_it->send_buff[p_it->now_send_len] = '\0';}
    if (p_it->before_sended_len < p_it->send_buff_len) {p_it->send_buff[p_it->before_sended_len] = '\0';}

    gettimeofday(&p_it->l_time[5],0);
    event_del(p_ev);

    if(p_it->before_sended_len >= p_it->now_send_len)
    {
        //send over
        snprintf(log_buff,sizeof(log_buff),
                "Thread[%d]\tClient[%d]\tsend_ok\t"
                "All_time[accept2recv_event=%d,recv=%d,parse_recv=%d,do_policy=%d,send=%d]"
                ":Send=%s",
                p_it->thread_id, fd, 
                my_time_diff(p_it->l_time[0], p_it->l_time[1]), 
                my_time_diff(p_it->l_time[1], p_it->l_time[2]), 
                my_time_diff(p_it->l_time[2], p_it->l_time[3]), 
                my_time_diff(p_it->l_time[3], p_it->l_time[4]), 
                my_time_diff(p_it->l_time[4], p_it->l_time[5]),
                p_it->send_buff);
        g_log.write_record(log_buff);
        //send success
        p_it->status = status_send;
        //reset
        p_it->reset();
        //regist recv event
        reg_add_event(p_ev,p_it->sd,EV_READ,RecvData,p_it,base,&tv_recv_timeout);
        //for press test
        //goto GC;
    }
    else
    {

        snprintf(log_buff,sizeof(log_buff),"Thread[%d]\tClient[%d]\tsend_error\t"
                "data[before_sended_len=%d,now_send_len=%d] error[%d]:%s",
                p_it->thread_id, fd, 
                p_it->before_sended_len, p_it->now_send_len, errno, strerror(errno));
        g_log.write_record(log_buff);
        goto GC;
    }
    return;
GC:
    close(p_ev->ev_fd);
    it_pool_put(p_it);
    return;
}

void RecvData(int fd, short int events, void *arg)
{
    if ( 0 == arg || fd <= 0)
        return;
    struct interface_data *p_it = (struct interface_data *)arg;
    struct event *p_ev = &(p_it->ev);
    struct event_base *base = p_ev->ev_base;
    char *buff = (char *)(p_it->recv_buff) + p_it->now_recv_len;
    int buff_len = p_it->recv_buff_len - p_it->now_recv_len;
    int len;
    char log_buff[BUFF_SIZE] = {0};

    //timeout
    if (events == EV_TIMEOUT)
    {
        close(p_ev->ev_fd);
        event_del(p_ev);
        it_pool_put(p_it);
        snprintf(log_buff,sizeof(log_buff),"recv timeout fd=[%d] error[%d]", fd, errno);
        g_log.write_record(log_buff);
        return;
    } 

    // receive data
    gettimeofday(&p_it->l_time[1],0);
    len = recv(fd,buff, buff_len, 0);
    gettimeofday(&p_it->l_time[2],0);
    event_del(p_ev);

    if(len > 0)
    {
        //read success
        p_it->status = status_recv;
        //regist send event
        p_it->now_recv_len += len;
        if (p_it->now_recv_len < p_it->recv_buff_len) {p_it->recv_buff[p_it->now_recv_len] = '\0';}
        int parse_ret = p_it->http.parse_done((char *)(p_it->recv_buff));

        snprintf(log_buff,sizeof(log_buff),"Thread[%d]\tClient[%d]:Recv=[%s]:Parse=[%d]",p_it->thread_id,fd, buff, parse_ret);
        g_log.write_record(log_buff);

        if (parse_ret < 0 )
        {goto GC;}
        else if (parse_ret == 0)	
        {reg_add_event(p_ev,p_it->sd,EV_READ,RecvData,p_it,base,&tv_recv_timeout);}
        else
        {
            gettimeofday(&p_it->l_time[3],0);
            p_it->policy.do_one_action(&p_it->http,p_it->send_buff,p_it->send_buff_len,p_it->now_send_len, p_it->recv_buff, p_it->recv_buff_len ,p_it->thread_id);
            gettimeofday(&p_it->l_time[4],0);
            SendData(p_it->sd,EV_WRITE,p_it);
            //			reg_add_event(p_ev,p_it->sd,EV_WRITE,SendData,p_it,base,NULL);
        }

    }
    else
    {
        if (len == 0)
        {
            snprintf(log_buff,sizeof(log_buff),"[fd=%d] , closed gracefully.", fd);
            g_log.write_record(log_buff);
        }
        else
        {	
            snprintf(log_buff,sizeof(log_buff),"recv[fd=%d] error[%d]:%s", fd, errno, strerror(errno));
            g_log.write_record(log_buff);
        }
        goto GC;
    }
    return;

GC:
    close(p_ev->ev_fd);
    it_pool_put(p_it);
    return;
}

void UpdateAcceptListen(int fd, short what, void *arg)
{
    struct sig_ev_arg *p_ev_arg = (struct sig_ev_arg *)arg;
    char log_buff[128] = {0};
    struct sockaddr_in sin;
    socklen_t len = sizeof(struct sockaddr_in);
    int accept_fd = 0;

    //one thread ,not need lock
    accept_fd = accept(fd, (struct sockaddr*)&sin, &len);

    if (accept_fd > 0)
    {
        int iret = 0;
        if((iret = fcntl(accept_fd, F_SETFL, O_NONBLOCK)) < 0)
        {   
            snprintf(log_buff,sizeof(log_buff),"%s fd=%d: fcntl nonblocking error:%d", __func__, accept_fd, iret);
            g_log.write_record(log_buff);
            return;
        }  
        // add a read event for receive data
        struct interface_data *p_read_it = it_pool_get();
        p_read_it->thread_id = p_ev_arg->thread_id;
        p_read_it->sd = accept_fd;
        gettimeofday(&p_read_it->l_time[0],0);
        reg_add_event(&(p_read_it->ev),accept_fd,EV_READ,RecvData,p_read_it, p_ev_arg->p_event_base, &tv_recv_timeout);
        snprintf(log_buff,sizeof(log_buff),"process thread read new client fd[%d]",accept_fd);
        g_log.write_record(log_buff);

    }
    else 
    {
        snprintf(log_buff,sizeof(log_buff),"%s[%d]\t%s:accept  error return:%d, errno:%d", __FILE__,__LINE__,__func__,accept_fd,errno);
        g_log.write_record(log_buff);
    }

}

void SearchAcceptListen(int fd, short what, void *arg)
{
    struct sig_ev_arg *p_ev_arg = (struct sig_ev_arg *)arg;

    char log_buff[128] = {0};
    struct sockaddr_in sin;
    socklen_t len = sizeof(struct sockaddr_in);
    int accept_fd = 0;

    //accept lock
    pthread_mutex_lock(&g_search_port_lock);
    accept_fd = accept(fd, (struct sockaddr*)&sin, &len);
    pthread_mutex_unlock(&g_search_port_lock);

    if (accept_fd > 0)
    {
        int iret = 0;
        if((iret = fcntl(accept_fd, F_SETFL, O_NONBLOCK)) < 0)
        {   
            snprintf(log_buff,sizeof(log_buff),"%s fd=%d: fcntl nonblocking error:%d", __func__, accept_fd, iret);
            g_log.write_record(log_buff);
            return;
        }  

        struct interface_data *p_read_it = it_pool_get();
        p_read_it->thread_id = p_ev_arg->thread_id;
        // add a read event for receive data
        p_read_it->sd = accept_fd;
        gettimeofday(&p_read_it->l_time[0],0);
        reg_add_event(&(p_read_it->ev),accept_fd,EV_READ,RecvData,p_read_it, p_ev_arg->p_event_base, &tv_recv_timeout);
        snprintf(log_buff,sizeof(log_buff),"Thread[%d] process thread read new client fd[%d]", p_read_it->thread_id, accept_fd);
        g_log.write_record(log_buff);

    }
    else 
    {
        snprintf(log_buff,sizeof(log_buff),"%s[%d]\t%s:accept  error return:%d, errno:%d", __FILE__,__LINE__,__func__,accept_fd,errno);
        g_log.write_record(log_buff);

    }

}

////////////////////////////////////////////

///////////process thread/////////////////

static void *ProcessThread(void *arg)
{
    if (0 == arg) pthread_exit(NULL);
    struct sig_ev_arg *p_ev_arg = (struct sig_ev_arg *)arg;
    struct event notify_event;
    struct event_base *base = event_init();
    p_ev_arg->p_event_base = base;

    reg_add_event(&notify_event,p_ev_arg->listen_fd,EV_READ | EV_PERSIST, SearchAcceptListen, p_ev_arg, base, NULL);
    event_base_dispatch(base);
    pthread_exit(NULL);
}

static void *UpdateProcessThread(void *arg)
{
    if (0 == arg) pthread_exit(NULL);
    struct sig_ev_arg *p_ev_arg = (struct sig_ev_arg *)arg;
    struct event notify_event;
    struct event_base *base = event_init();
    p_ev_arg->p_event_base = base;

    reg_add_event(&notify_event,p_ev_arg->listen_fd,EV_READ | EV_PERSIST, UpdateAcceptListen, p_ev_arg, base, NULL);
    event_base_dispatch(base);
    pthread_exit(NULL);
}

////////////////////////////////////////////

//succ return 0,other return < 0

int parse_config(char *config_path)
{
    int ret = 0;
    if (0 == config_path) {return -1;}
    Json::Reader reader;  
    Json::Value root;
    std::ifstream is;
    is.open(config_path);

    if (!is.is_open())
    {
        ret = -2;
    }

    if (reader.parse(is, root))  
    {
        if (!root["recv_buff"].isNull())
        {
            int value = root["recv_buff"].asInt();
            if (value > 50 && value < (1024*1024*100))
            {g_recv_buff = value;}
        } 
        if (!root["send_buff"].isNull())
        {
            int value = root["send_buff"].asInt();
            if (value > 50 && value < (1024*1024*100))
            {g_send_buff = value;}
        } 
        if (!root["search_port"].isNull())
        {
            int value = root["search_port"].asInt();
            if (value > 0 && value < (65536))
            {g_search_port = (short)value;}
        } 
        if (!root["update_port"].isNull())
        {
            int value = root["update_port"].asInt();
            if (value > 0 && value < (65536))
            {g_update_port = (short)value;}
        } 
        if (!root["search_ip"].isNull() && root["search_ip"].isString())
        {
            std::string value = root["search_ip"].asString();
            memcpy(g_search_ip,value.c_str(),value.size() > 32 ? 32: value.size() );
        } 
        if (!root["update_ip"].isNull() && root["update_ip"].isString())
        {
            std::string value = root["update_ip"].asString();
            memcpy(g_update_ip,value.c_str(),value.size() > 32 ? 32: value.size() );
        } 
        if (!root["listen_backlog"].isNull())
        {
            int value = root["listen_backlog"].asInt();
            if (value > -2)
            {g_listen_backlog = value;}
        } 
        if (!root["tv_recv_timeout_sec"].isNull())
        {
            int value = root["tv_recv_timeout_sec"].asInt();
            if (value >= 0)
            {tv_recv_timeout.tv_sec = value;}
        } 
        if (!root["tv_recv_timeout_usec"].isNull())
        {
            int value = root["tv_recv_timeout_usec"].asInt();
            if (value >= 0)
            {tv_recv_timeout.tv_usec = value;}
        }
        if (!root["search_thread_num"].isNull())
        {
            int value = root["search_thread_num"].asInt();
            if (value >= 1 && value < MAX_PROCESS_THREAD)
            {g_search_thread_num = value;}
        } 
        if (!root["index_storage_file"].isNull() &&
                root["index_storage_file"].isString())
        {
            g_index_parent_file = root["index_storage_file"].asString();
        }
        if (!root["index_key_load_file"].isNull() &&
                root["index_key_load_file"].isString())
        {
            g_key_load_file = root["index_key_load_file"].asString();
        }
        if (!root["index_key_dump_file"].isNull() &&
                root["index_key_dump_file"].isString())
        {
            g_key_dump_file = root["index_key_dump_file"].asString();
        }
    }
    else
    {
        ret = -3;
    }

    is.close();
    return ret;	
}

int InitListenSocket(char *ip, short port) 
{
    if (0 == ip) return 0;

    int listenFd = socket(AF_INET, SOCK_STREAM, 0); 
    int ret = 0;
    char log_buff[128] = {0};
    fcntl(listenFd, F_SETFL, O_NONBLOCK); // set non-blocking 
    // bind & listen 
    sockaddr_in sin; 
    bzero(&sin, sizeof(sin)); 
    sin.sin_family = AF_INET; 
    sin.sin_addr.s_addr = inet_addr(ip);
    sin.sin_port = htons(port); 
    bind(listenFd, (const sockaddr*)&sin, sizeof(sin)); 
    ret = listen(listenFd, g_listen_backlog); 
    snprintf(log_buff,sizeof(log_buff),"server listen fd=%d port=%d retrun:%d errno:%d", listenFd,port,ret,errno);
    g_log.write_record(log_buff);
    return listenFd;
}

int main (int argc, char **argv)
{
    char log_buff[128] = {0};

    //parse config
    if (argc == 2)
    {
        int ret = parse_config(argv[1]);
        snprintf(log_buff,sizeof(log_buff),"parse_config[%s] return:%d",argv[1],ret);
        g_log.write_record(log_buff);

    }

    //policy_interface_init_once
    bool ret = policy_interface_init_once();
    snprintf(log_buff,sizeof(log_buff),"policy_interface_init_once return:%d errno:%d",ret,errno);
    g_log.write_record(log_buff);
    if (!ret)
    {
        snprintf(log_buff,sizeof(log_buff),"policy_interface_init_once error,so exit");
        g_log.write_record(log_buff);
        return 0;
    }

    // init search and update port
    int old_errno = errno;
    errno = 0;
    int search_port_sock = InitListenSocket(g_search_ip, g_search_port); 
    int update_port_sock = InitListenSocket(g_update_ip, g_update_port);
    errno = old_errno;
    //check sock
    if (search_port_sock <= 0 || update_port_sock <= 0)
    {
        snprintf(log_buff,sizeof(log_buff),"search[%d] or update[%d] sock listen error[%d]",search_port_sock,update_port_sock,errno);
        g_log.write_record(log_buff);
        return -1;
    }

    //create threads
    for(int i = 0 ; i < g_search_thread_num;i++)
    {
        notify_arg[i].listen_fd = search_port_sock;
        notify_arg[i].thread_id = ++g_now_max_thread_id;

        int err = pthread_create(&ptocess_thread[i],0,ProcessThread,&notify_arg[i]);
        if (err != 0)
        {
            snprintf(log_buff,sizeof(log_buff),"can't create ProcessThread thread:%s",strerror(err));
            g_log.write_record(log_buff);
            return -1;
        }
    }
    //create update threads
    for(int i = 0 ; i < UPDATE_PROCESS_THREAD;i++)
    {
        update_notify_arg[i].listen_fd = update_port_sock;
        update_notify_arg[i].thread_id = ++g_now_max_thread_id;

        int err = pthread_create(&update_ptocess_thread[i],0,UpdateProcessThread,&update_notify_arg[i]);
        if (err != 0)
        {
            snprintf(log_buff,sizeof(log_buff),"can't create Update ProcessThread thread:%s",strerror(err));
            g_log.write_record(log_buff);
            return -1;
        }
    }

    for (int i = 0 ; i < g_search_thread_num;i++)
    {
        pthread_join(ptocess_thread[i],0);
    }
    for (int i = 0 ; i < UPDATE_PROCESS_THREAD ;i++)
    {
        pthread_join(update_ptocess_thread[i],0);
    }
    return (0);
}

