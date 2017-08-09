#ifndef RTMP_FLOW_H
#define RTMP_FLOW_H
#include <pthread.h>
#include <list>
#include <map>
#include <time.h>

using namespace std;

struct rtmppkt
{
    char* buf;
    int size;
};

enum {
    RTMP_HANDSHAKE = 0,
    RTMP_CONNECT,
    RTMP_PUBLISH,
    RTMP_PUSHING,
};


class rtmpparser : public appplugin {
public:

    rtmpparser(const flow_addr &flow_addr_, bool sync_) :
        appplugin(flow_addr_, sync_),
        flowinfo(addr.str()),
        sockfd(-1),
        processed_size(0),
        pkt_buf(NULL),
        pkt_buf_size(0),
        pkt_buf_max_size(0),
        chunk_size(128),
        expect_pkt_buf_size(4),
        payload_size(0),
        running(false),
        status(RTMP_HANDSHAKE),
        start_time(0)
    {
        pkt_buf = (char*)malloc(128*1024);
        pkt_buf_max_size = 1024 * 128;
        pthread_mutex_init(&lock, NULL);
        memset(&thread, 0, sizeof(thread));
    }

    ~rtmpparser();
    
    virtual int init();

    virtual int process_packet(const char* buf, size_t size);

    virtual void stop();

    virtual void wait_exit();

    void main_loop();

private:
    int send_data(const char* buf, size_t size);
    int recv_data(char** buf, int* size, int wait);
    void send_pkt(const char* pkt_buf, int pkt_buf_size, const char* payload_buf, int payload_buf_size);

    int do_process(const char* buf, size_t size);
    int parse_packet(char* buf, size_t size);
    void die();
    void post_vod_playlist();
    
    int do_http_request(const std::string& method, const std::string& url, std::map<string, std::string>& header, std::string& response);
    std::string gen_publish_url();

    char* replace_buf(const char* buf, int buf_size, const char* org, int org_size, const char* to, int to_size);
    int convert_to_payload_buf(const char* buf, int size, char** payload_buf, int* payload_size);

private:

    std::string flowinfo;
    int sockfd;
    int processed_size;
    char* pkt_buf;
    int pkt_buf_size;
    int pkt_buf_max_size;
    int chunk_size;
    int expect_pkt_buf_size;
    int payload_size;

    bool running;
    int status;
    time_t start_time;

    std::string channel_id;
    std::string rtmp_url;
    std::list<rtmppkt> pkt_list;
    pthread_mutex_t lock;
    pthread_t thread;
};

#endif
