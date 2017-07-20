#ifndef RTMP_FLOW_H
#define RTMP_FLOW_H
#include <pthread.h>
#include <list>

struct rtmppkt
{
    char* buf;
    int size;
};

enum {
    RTMP_HANDSHAKE = 0,
    RTMP_CONNECT,
    RTMP_PUBLISH,
};


class rtmpparser : public appplugin {
public:

    rtmpparser(const flow_addr &flow_addr_, bool sync) :
        appplugin(flow_addr_, sync),
        sockfd(-1),
        processed_size(0)
    {
        flowinfo = addr.str();
        pkt_buf = (char*)malloc(128*1024);
        pkt_buf_size = 0;
        pkt_buf_max_size = 1024 * 128;
        expect_pkt_buf_size = 1;
        payload_size = 0;
        pthread_mutex_init(&lock, NULL);
        running = false;
        status = RTMP_HANDSHAKE;
    }

    ~rtmpparser()
    {
        close(sockfd);
        free(pkt_buf);
    }
    
    virtual int init();

    virtual int process_packet(const char* buf, size_t size);

    virtual void destroy();

    void main_loop();

private:
    int send_data(const char* buf, size_t size);
    int recv_data(char** buf, int* size, int wait);
    int do_process(const char* buf, size_t size);
    int parse_packet(char* buf, size_t size);
    void send_pkt(const char* pkt_buf, int pkt_buf_size, const char* payload_buf, int payload_buf_size);
    void die();

private:

    std::string flowinfo;
    int sockfd;
    int processed_size;
    char* pkt_buf;
    char* payload_buf;
    int pkt_buf_size;
    int pkt_buf_max_size;
    int expect_pkt_buf_size;
    int payload_size;
    std::list<rtmppkt> pkt_list;
    pthread_mutex_t lock;
    bool running;
    std::string rtmp_url;
    int status;
};

#endif
