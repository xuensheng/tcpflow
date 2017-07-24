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

    ~rtmpparser();
    
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
    int do_http_request(const std::string& method, const std::string& url, std::map<string, std::string>& header, std::string& response);
    std::string gen_publish_url();
    char* replace_buf(const char* buf, int buf_size, const char* org, int org_size, const char* to, int to_size);
    int convert_to_payload_buf(const char* buf, int size, char** payload_buf, int* payload_size);

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
    time_t start_time;
    std::string channel_id;
};

#endif
