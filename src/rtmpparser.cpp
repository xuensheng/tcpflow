#include "tcpflow.h"
#include "tcpip.h"
#include "rtmpparser.h"

#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <map>
#include <curl/curl.h>

using namespace std;

void* process_packet_worker(void* args)
{
    rtmpparser* parser = (rtmpparser*)args;

    parser->main_loop();
}

int rtmpparser::init()
{
    //创建socket，用于连接oss_server
    if((sockfd = socket(AF_INET,SOCK_STREAM,0)) == -1) {
	    DEBUG(1)("unable to open socket, error: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr,sizeof(server_addr));
    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(5391);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if(connect(sockfd, (struct sockaddr *)(&server_addr), sizeof(struct sockaddr)) == -1) {
	    DEBUG(1)("unable to connect socket, error: %s", strerror(errno));
        return -1;
    }

    running = true;

    //创建线程，用于replay rtmp packets
    pthread_create(&thread, NULL, process_packet_worker, this);

    return 0;
}

void rtmpparser::stop()
{
	DEBUG(1)("stream closed");

    running = false;
}

void rtmpparser::wait_exit()
{
    void* tret;
    pthread_join(thread, &tret); 
}

rtmpparser::~rtmpparser()
{
    if (sockfd > 0);
        close(sockfd);

    if (pkt_buf)
        free(pkt_buf);

    if (status >= RTMP_PUBLISH) {
        //等待oss写完文件
        sleep(5);

        //生成vod播放列表
        string response;
        map<string, string> header;
        char start_time_str[12];
        sprintf(start_time_str, "%ld", start_time);
        header["x-oss-start-time"] = start_time_str;
        header["x-oss-channel-id"] = channel_id;
        int ret = do_http_request("POST", "http://127.0.0.1:7123/publishurl", header, response);
        if (ret == 0 && !response.empty())
            DEBUG(1)("post succeed: %s", response.c_str());
        else
            DEBUG(1)("post failed : %s", response.c_str());

    }
}

//worker线程主循环
void rtmpparser::main_loop()
{
    rtmppkt pkt;
    while(1)
    {
        pthread_mutex_lock(&lock);
        if (pkt_list.empty()) {
            pthread_mutex_unlock(&lock);
            if (!running) {
                break;
            }
            usleep(20 * 1000);
            continue;
        }

        pkt = pkt_list.back();
        pkt_list.pop_back();
        pthread_mutex_unlock(&lock);
        
        do_process(pkt.buf, pkt.size);
        delete pkt.buf;
    }
	DEBUG(1)("end process");
}

//发送数据到oss
int rtmpparser::send_data(const char* buf, size_t size)
{
    int ret;
    int left = size;

    //clear recv buf
    char recv_buf[16*1024];
    do {
        ret = recv(sockfd, recv_buf, 16 * 1024, MSG_DONTWAIT);
        if (ret == 0) {
            usleep(100*1024);
            ret = recv(sockfd, recv_buf, 16 * 1024, MSG_DONTWAIT);
        }
    } while(ret > 0);

    while(left > 0) {
        ret = send(sockfd, buf + size - left, left, 0);
        if ( ret < 0) {
            close(sockfd);
            sockfd = -1;
            return -1;
        } else if (ret != left) {
            usleep(10*1000);
            continue;
        }

        left -= ret;
    }
    DEBUG(1)("send pkt size: %d ", size); 
    //控制发送速率 < 1MB/s
    usleep(size);
    return 0;
}

//从oss接收数据
int rtmpparser::recv_data(char** buf, int* size, int wait)
{
    sleep(wait);

    char * recv_buf = new char[16*1024];
    int len = 0;
    int ret;
    do {
        ret = recv(sockfd, recv_buf + len, 16 * 1024 - len, MSG_DONTWAIT);
        if (ret > 0)
            len += ret;
        if (len >= 16 * 1024)
            break;
    } while(ret > 0);

    if (buf != NULL && size != NULL) {
        *size = len;
        *buf = recv_buf;
    } else {
        delete recv_buf;
    }

    return 0;
}

struct rtmp_header {
    unsigned char type;
    unsigned char ts[3];
    unsigned char amf_size[3];
    unsigned char amf_type;
    unsigned char stream_id[4];
};

typedef enum {                       
    AMF_DATA_TYPE_NUMBER      = 0x00,
    AMF_DATA_TYPE_BOOL        = 0x01,
    AMF_DATA_TYPE_STRING      = 0x02,
    AMF_DATA_TYPE_OBJECT      = 0x03,
    AMF_DATA_TYPE_NULL        = 0x05,
    AMF_DATA_TYPE_UNDEFINED   = 0x06,
    AMF_DATA_TYPE_REFERENCE   = 0x07,
    AMF_DATA_TYPE_MIXEDARRAY  = 0x08,
    AMF_DATA_TYPE_OBJECT_END  = 0x09,
    AMF_DATA_TYPE_ARRAY       = 0x0a,
    AMF_DATA_TYPE_DATE        = 0x0b,
    AMF_DATA_TYPE_LONG_STRING = 0x0c,
    AMF_DATA_TYPE_UNSUPPORTED = 0x0d,
} AMFDataType;                       

static uint16_t AV_RB16(const uint8_t *p)
{
    return (p[0] << 8)  + p[1];
}

static uint32_t AV_RB24(const uint8_t *p)
{
    return (p[0] << 16)  + (p[1] << 8) + p[2];
}

static uint32_t AV_RB32(const uint8_t *p)
{
    return (p[0] << 24)  + (p[1] << 16) + (p[2] << 8) + p[3];
}

static void AV_WB16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

static void AV_WB24(uint8_t *p, uint32_t v)
{
    p[0] = v >> 16;
    p[1] = (v >> 8) & 0xff;
    p[2] = v & 0xff;
}

#define DEF(type, name, bytes, read)                                           \
static type bytestream_get_ ## name(const uint8_t **b)        \
{                                                                              \
    (*b) += bytes;                                                             \
    return read(*b - bytes);                                                   \
}        

DEF(unsigned int, be16, 2, AV_RB16)
DEF(unsigned int, be32, 4, AV_RB32)

#define min(a,b) ((a) < (b) ? (a) : (b))

int ff_amf_tag_size(const uint8_t *data, const uint8_t *data_end)
{
    const uint8_t *base = data;
    AMFDataType type;
    unsigned nb   = -1;
    int parse_key = 1;

    if (data >= data_end)
        return -1;
    int t = *data++;
    switch ((type = (AMFDataType)t)) {
    case AMF_DATA_TYPE_NUMBER:      return 9;
    case AMF_DATA_TYPE_BOOL:        return 2;
    case AMF_DATA_TYPE_STRING:      return 3 + AV_RB16(data);
    case AMF_DATA_TYPE_LONG_STRING: return 5 + AV_RB32(data);
    case AMF_DATA_TYPE_NULL:        return 1;
    case AMF_DATA_TYPE_DATE:        return 11;
    case AMF_DATA_TYPE_ARRAY:
        parse_key = 0;
    case AMF_DATA_TYPE_MIXEDARRAY:
        nb = bytestream_get_be32(&data);
    case AMF_DATA_TYPE_OBJECT:
        while (nb-- > 0 || type != AMF_DATA_TYPE_ARRAY) {
            int t;
            if (parse_key) {
                int size = bytestream_get_be16(&data);
                if (!size) {
                    data++;
                    break;
                }
                if (size < 0 || size >= data_end - data)
                    return -1;
                data += size;
            }
            t = ff_amf_tag_size(data, data_end);
            if (t < 0 || t >= data_end - data)
                return -1;
            data += t;
        }
        return data - base;
    case AMF_DATA_TYPE_OBJECT_END:  return 1;
    default:                        return -1;
    }
}

int ff_amf_get_field_value(const uint8_t *data, const uint8_t *data_end,
                           const uint8_t *name, uint8_t *dst, int dst_size)
{
    int namelen = strlen((const char*)name);
    int len;

    while (*data != AMF_DATA_TYPE_OBJECT && data < data_end) {
        len = ff_amf_tag_size(data, data_end);
        if (len < 0)
            len = data_end - data;
        data += len;
    }
    if (data_end - data < 3)
        return -1;
    data++;
    for (;;) {
        int size = bytestream_get_be16(&data);
        if (!size)
            break;
        if (size < 0 || size >= data_end - data)
            return -1;
        data += size;
        if (size == namelen && !memcmp(data-size, name, namelen)) {
            switch (*data++) {
            case AMF_DATA_TYPE_NUMBER:
                break;
            case AMF_DATA_TYPE_BOOL:
                break;
            case AMF_DATA_TYPE_STRING:
                len = bytestream_get_be16(&data);
                strncpy((char*)dst, (char*)data, min(len+1, dst_size));
                dst[min(len+1, dst_size)] = '\0';
                break;
            default:
                return -1;
            }
            return 0;
        }
        len = ff_amf_tag_size(data, data_end);
        if (len < 0 || len >= data_end - data)
            return -1;
        data += len;
    }
    return -1;
}

size_t write_data(void * ptr, size_t size, size_t nmemb, void * stream)
{
    memcpy(stream, ptr, size * nmemb);
    return size * nmemb;
}

//发送http请求，创建livechannel生成推流地址或者生成播放列表
int rtmpparser::do_http_request(const string& method, const string& url, map<string, string>& header, string& response)
{  
    CURL * curl;

    char buff[2048];
    memset(buff, 0, sizeof(buff));

    curl_global_init(CURL_GLOBAL_ALL);

    curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20);

    if(method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "postvodlist"); 
    }

    struct curl_slist *headers = NULL;
    for(map<string, string>::iterator it = header.begin(); it != header.end(); ++it) {
        string headstr = it->first + ": " + it->second;
        headers = curl_slist_append(headers, headstr.c_str());
    }

    if (headers != NULL)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buff);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    printf("\nbuff : %s\n", buff);

    response = buff;

    return 0;
}

//获取推流地址,格式： rtmp://bucket.endpoint/live/channel_id?xxxxxxxx
std::string rtmpparser::gen_publish_url()
{
    string response;
    map<string, string> header;
    int ret = do_http_request("GET", "http://127.0.0.1:7123/publishurl", header, response);

    if (ret == 0)
        return response;

    return "";
}

//替换原始的rtmp流
char* rtmpparser::replace_buf(const char* buf, int buf_size, const char* org, int org_size, const char* to, int to_size)
{
    int new_size = buf_size + to_size;
    char* new_buf = new char[new_size];

    DEBUG(1)("%s replace: %s to %s", flowinfo.c_str(), org, to);

    int front = org - buf;
    int mid = org_size;
    int back = buf_size - front - mid;

    assert(front > 0 && front <= buf_size);

    int len = org - buf;
    memcpy(new_buf, buf, front);
    memcpy(new_buf + front, to, to_size);
    memcpy(new_buf + front + to_size, org + org_size, back);

    return new_buf;
}

const char* memstr(const char* buf, int buf_size, const char* str)
{
    int len = strlen(str);
    for (int i = 0; i < buf_size - len; i++) {
        if (memcmp(buf + i, str, len) == 0)
            return buf + i;
    }

    return NULL;
}

int get_header_size(unsigned char type)
{
    int fmt = (type & 0xc0) >> 6;
    int header_size;

    if (fmt == 0)
        header_size = 12;
    else if (fmt == 1)
        header_size = 8;
    else if (fmt == 2)
        header_size = 4;
    else if (fmt = 3)
        header_size = 1;

    return header_size;
}

//异步/同步处理packets
int rtmpparser::process_packet(const char* buf, size_t size)
{
    if (sockfd < 0 || size < 1)
        return -1;

    DEBUG(1)("capture pkt size: %d", size);

    {
        char* new_buf = new char[size];
        memcpy(new_buf, buf, size);

        rtmppkt pkt;
        pkt.buf = new_buf;
        pkt.size = size;

        pthread_mutex_lock(&lock);
        if (sync) {
            while (pkt_list.size() > 10) {
                pthread_mutex_unlock(&lock);
                usleep(1000*10);
                pthread_mutex_lock(&lock);
            }
        }
        pkt_list.push_front(pkt);
        pthread_mutex_unlock(&lock);
    }
}

//解析packets主逻辑，主要是解析原始的rtmp流
int rtmpparser::do_process(const char* buf, size_t size)
{
    if (sockfd < 0 || size < 1)
        return -1;

    DEBUG(1)("process pkt size: %d", size);

    const char* orgbuf = buf;

    //握手消息3073大小
    if (processed_size < 3073) {
        send_data(buf, size);
        processed_size += size;
        return 0;
    }

    //pulish成功，直接发送数据，不再解析
    if (status >= RTMP_PUBLISH) {
        send_data(buf, size);
        return 0;
    }

    //解析rtmp
    while(size > 0) {
        if (expect_pkt_buf_size > pkt_buf_max_size) {
            pkt_buf = (char*)realloc(pkt_buf, expect_pkt_buf_size);
            pkt_buf_max_size = expect_pkt_buf_size;
        }

        DEBUG(1)("expect_pkt_buf_size: %d pkt_buf_size: %d",
            expect_pkt_buf_size, pkt_buf_size);

        int copy_size = min(expect_pkt_buf_size - pkt_buf_size, size);
        memcpy(pkt_buf + pkt_buf_size, buf, copy_size);
        int parsed = buf - orgbuf;
        buf += copy_size;
        size -= copy_size;
        pkt_buf_size += copy_size;

        if(pkt_buf_size < expect_pkt_buf_size) {
            continue;
        }

        assert(pkt_buf_size == expect_pkt_buf_size);

        rtmp_header* head = (rtmp_header *)pkt_buf;
        int header_size = get_header_size(head->type);
        int body_size = 0;

        if (pkt_buf_size < header_size) {
            expect_pkt_buf_size = header_size;
            continue;
        }
        
        if (header_size < 8) {
            send_data(pkt_buf, pkt_buf_size);
            processed_size += pkt_buf_size;
            pkt_buf_size = 0;
            continue;
        }

        body_size = AV_RB24(head->amf_size);

        //接收到rtmp header
        if (header_size >= 8 && pkt_buf_size == header_size) {
            DEBUG(1)("process pkt body size: %d", body_size);
            if (body_size <= 128) {
                expect_pkt_buf_size = header_size + body_size;
                payload_size = body_size;
            } else {
                expect_pkt_buf_size = header_size + 128 + 1;
                payload_size = 128;
            }

            continue;
        }
        
        if (payload_size < body_size) {
            //解析下一个chunk header
            rtmp_header* head2 = (rtmp_header *)&pkt_buf[pkt_buf_size - 1];
            int header_size2 = get_header_size(head2->type);
            assert(header_size2 == 1);
            int left = body_size - payload_size;
            if (left <= 128) {
                expect_pkt_buf_size += (header_size2 - 1) + left;
            } else {
                expect_pkt_buf_size += (header_size2 - 1) + 128 + 1;
            }
            payload_size += min(left, 128);
            continue;
        } else {
            //解析完一个message
            parse_packet(pkt_buf, pkt_buf_size);
            processed_size += pkt_buf_size;
            pkt_buf_size = 0;
            expect_pkt_buf_size = 1;
            if (status >= RTMP_PUBLISH) {
                if (size > 0)
                    send_data(buf, size);
                return 0;
            }
        }
    }
}

//packet buf 转换为 payload buf
int rtmpparser::convert_to_payload_buf(const char* buf, int size, char** payload_buf, int* payload_size)
{
    int pos = 0;
    int new_size = 0;
    char* new_buf = new char[size];
    rtmp_header* head = (rtmp_header *)(buf + pos);
    int total_body_size = AV_RB24(head->amf_size);

    do {
        head = (rtmp_header *)(buf + pos);
        int header_size = get_header_size(head->type);
        int body_size = min(total_body_size - new_size, 128);
        pos += header_size;
        memcpy(new_buf + new_size, buf + pos, body_size);
        pos += body_size;
        new_size += body_size;
    } while(pos <= size);

    *payload_buf = new_buf;
    *payload_size = new_size;

    return 0;
}

void rtmpparser::send_pkt(const char* pkt_buf, int pkt_buf_size, const char* payload_buf, int payload_buf_size)
{
    char* new_pkt_buf = new char[pkt_buf_size + 1024];
    int new_pkt_buf_size = 0;
    rtmp_header* head = (rtmp_header *)pkt_buf;
    int header_size = get_header_size(head->type);

    memcpy(new_pkt_buf, pkt_buf, header_size);
    new_pkt_buf_size += header_size;
    char chunk_header = pkt_buf[0] | 0xc0;
    do {
        int send_size = min(payload_buf_size, 128);
        memcpy(new_pkt_buf + new_pkt_buf_size, payload_buf, send_size);
        payload_buf_size -= send_size;
        new_pkt_buf_size += send_size;
        payload_buf += send_size;
        if (payload_buf_size > 0) {
            memcpy(new_pkt_buf + new_pkt_buf_size, &chunk_header, 1);
            new_pkt_buf_size++;
        }
    } while(payload_buf_size > 0);

/*
    if (new_pkt_buf_size != pkt_buf_size)
        abort();

    if (memcmp(pkt_buf, new_pkt_buf, new_pkt_buf_size) != 0)
        abort();
*/
    send_data(new_pkt_buf, new_pkt_buf_size);

    delete new_pkt_buf;
}

struct amf_string {
    unsigned char type;
    unsigned char size[2];
    char str[0];
};

bool check_cmd(const char* buf, const char* cmd)
{
    const amf_string* amf_str = (const amf_string *)buf;
    return (amf_str->type == 0x02
        && AV_RB16(amf_str->size) == strlen(cmd)
        && strncmp(amf_str->str, cmd, strlen(cmd)) == 0);
}

int rtmpparser::parse_packet(char* buf, size_t size)
{

    rtmp_header* head = (rtmp_header *)buf;
    int header_size = get_header_size(head->type);
    
    int amf_type = head->amf_type;
    int amf_size = (head->amf_size[0] << 16) + (head->amf_size[1] << 8) + head->amf_size[2];

    DEBUG(1)("pkt size: %d amf type: 0x%x body size: %d", size, amf_type, amf_size); 

    if (amf_type != 0x14) {
        send_data(buf, size);
        return 0;
    }
    
    char *payload_buf = NULL;
    int payload_buf_size = 0;
    convert_to_payload_buf(buf, size, &payload_buf, &payload_buf_size);

    amf_string* amf_str = (amf_string *)(payload_buf + header_size);
    
    if (check_cmd(payload_buf, "connect")) {
        const char* pos = memstr(payload_buf, payload_buf_size, "tcUrl");
        if (pos != NULL) {
            //rewrite tcurl
            pos = pos + strlen("tcUrl");
            if(pos[0] != 0x02) {//type string
                die();
                DEBUG(1)("unexpect publish msg");
                delete payload_buf;
                return 0;
            }
            pos++;
            int url_size = AV_RB16((uint8_t*)pos);

            rtmp_url = gen_publish_url();
            if (rtmp_url == "") {
                die();
                DEBUG(1)("unable to gen publish url");
                delete payload_buf;
                return 0;
            }

            int end_pos = rtmp_url.find_last_of("/");
            std::string connect_url = rtmp_url.substr(0, end_pos);
            channel_id = rtmp_url.substr(end_pos + 1, rtmp_url.find("?") - end_pos - 1);
            //std::string connect_url = "rtmp://xes-test-live-channel.oss-test.aliyun-inc.com:1935/live";
            int connect_url_size = connect_url.length();

            char* new_payload_buf = replace_buf(payload_buf, payload_buf_size, pos + 2, url_size, connect_url.c_str(), connect_url_size);
            int new_payload_size = payload_buf_size - url_size + connect_url_size;

            AV_WB24((uint8_t*)head->amf_size, new_payload_size);
            AV_WB16((uint8_t*)new_payload_buf + (pos - payload_buf), connect_url_size);

            send_pkt(buf, size, new_payload_buf, new_payload_size);
            delete payload_buf;
            delete new_payload_buf; 

            status = RTMP_CONNECT;
            return 0;
        }
        die();
        DEBUG(1)("unexpect connect msg");
        delete payload_buf;
        return 0;
    } else if (check_cmd(payload_buf, "publish")) {
        const char* pos = payload_buf + 3 + strlen("publish") + 10;
            //rewrite publish channel
        if (pos[0] == 0x02) {
            pos++;
            int url_size = AV_RB16((uint8_t*)pos);

            int start_pos = rtmp_url.find_last_of("/") + 1;
            std::string publish_url = rtmp_url.substr(start_pos, rtmp_url.length() - start_pos);
            //std::string publish_url = "test_rtmp_live_1500451501?OSSAccessKeyId=LTAIdrzDuhBJeJfA&playlistName=test.m3u8&Expires=1500811501&Signature=YdNg2SykcKEclAwljWh9Da8kiXg%3D";
            int publish_url_size = publish_url.length();

            char* new_payload_buf = replace_buf(payload_buf, payload_buf_size, pos + 2, url_size, publish_url.c_str(), publish_url_size);
            int new_payload_size = payload_buf_size - url_size + publish_url_size;

            AV_WB24((uint8_t*)head->amf_size, new_payload_size);
            AV_WB16((uint8_t*)new_payload_buf + (pos - payload_buf), publish_url_size);

            recv_data(NULL, NULL, 1);
            send_pkt(buf, size, new_payload_buf, new_payload_size);
            delete payload_buf;
            delete new_payload_buf; 

            char* recv_buf;
            int recv_size = 0;
            recv_data(&recv_buf, &recv_size, 1);
            if (recv_size > 0) {
                char code_buf[1024];
                rtmp_header* recv_head = (rtmp_header *)recv_buf;
                ff_amf_get_field_value((const uint8_t*)(recv_buf + get_header_size(recv_head->type)),
                        (const uint8_t*)(recv_buf + recv_size),
                        (const uint8_t*)"code",
                        (uint8_t*)code_buf,
                        sizeof(code_buf));  
                DEBUG(1)("publish response code: %s", code_buf);
                delete recv_buf;
                if (strncmp(code_buf, "NetStream.Publish.Start", strlen("NetStream.Publish.Start") != 0)) {
                    die();
                    DEBUG(1)("publish failed");
                    return 0;
                }
            } else {
                    die();
                    DEBUG(1)("recv publish response failed");
                    return 0;
            }
            
            start_time = time(NULL);
            status = RTMP_PUBLISH;
            return 0;
        }
        die();
        DEBUG(1)("unexpect publish msg");
        delete payload_buf;
        return 0;
    }

    delete payload_buf;

    if (status >= RTMP_CONNECT) {
        send_data(buf, size);
    } else {
        die();
        DEBUG(1)("unexpect msg");
    }

    return 0;
}

void rtmpparser::die()
{
    close(sockfd);
    sockfd = -1;
}
