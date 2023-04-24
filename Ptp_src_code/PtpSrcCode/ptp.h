#ifndef PTP_H
#define PTP_H
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QUdpSocket>
#include <iostream>
#ifdef Q_OS_LINUX
#    include <time.h>
#else
#    include <windows.h>
#endif
struct PtpHeader {
    char transportSpecific_messageType;  // 0-3bit messageType,4-7bit是transportSpecific
    char reserved_versionPTP;            // 0-3bit versionPtp,4-7bit是reserved
    unsigned short messageLength;        //报文长度
    unsigned char domainNumber;          //域号
    unsigned char reserved_0;
    unsigned short flagField;  //标识位
    char correctionField[8];  //修正域，各报文都有，主要用在 Sync 报文中，用于补偿网络中的传输时延，E2E 的频率同步
    char reserved1[4];
    char sourcePortIdentity[10];  //源端口标识符，发送该消息时钟的 ID 和端口号
    unsigned short sequenceId;    //帧序号
    char controlField;            //控制类型由messageType决定
    char logMessageInterval;      //消息间隔

    char send_time[10];               // follow up 带有时间长度
    char requestingPortIdentity[10];  // Delay_Resp 响应 Delay_Req 报文的发送设备端口 ID
    PtpHeader() {}
};

//表示消息类型。1588V2消息分为两类：事件消息（EVENT Message）和通用消息（General
// Message）。事件报文是时间概念报文，进出设备端口时需要打上精确的时间戳，而通用报文则是非时间概念报文，进出设备不会产生时戳。

//类型值0~3的为事件消息，8~D为通用消息。
// messageType
// 0x00: Sync
// 0x01: Delay_Req
// 0x02: Pdelay_Req
// 0x03: Pdelay_Resp

// 0x04-7: Reserved
// 0x08: Follow_Up
// 0x09: Delay_Resp
// 0x0A: Pdelay_Resp_Follow_Up
// 0x0B: Announce
// 0x0C: Signaling
// 0x0D: Management
// 0x0E-0x0F: Reserved

BOOL EnableSetTimePriviledge();

#define PTP_EVENT_PORT 319
#define PTP_GENERAL_PORT 320
#define DEFAULT_PTP_DOMAIN_ADDRESS "224.0.1.129"

#define PTP_ANNOUNCE 0x0B
#define PTP_SYNC 0x00
#define PTP_FOLLOW_UP 0x08
#define PTP_DELAY_REQ 0x01
#define PTP_DELAY_RESP 0x09

#define SECOND_DIV_NASECOND 1000000000

struct Nanosecond {
    long long seconds;
    long na_seconds;
    void reset() {
        seconds = 0;
        na_seconds = 0;
    }
    Nanosecond operator+(const Nanosecond& t) const {
        Nanosecond sum;
        long na_res = na_seconds + t.na_seconds;
        sum.na_seconds = na_res % SECOND_DIV_NASECOND;
        int add_seconds = na_res / SECOND_DIV_NASECOND;
        sum.seconds = t.seconds + add_seconds + seconds;

        return sum;
    }
    Nanosecond operator-(const Nanosecond& t) const {
        Nanosecond sub;
        long na_res = na_seconds - t.na_seconds;
        int sub_sec = 0;
        if (na_res < 0) {
            na_res = na_seconds + SECOND_DIV_NASECOND - t.na_seconds;
            sub_sec = 1;
        }
        sub.na_seconds = na_res % SECOND_DIV_NASECOND;

        sub.seconds = seconds - sub_sec - t.seconds;
        return sub;
    }
    Nanosecond operator/(int div_) const {
        Nanosecond res;
        long long ns = seconds * SECOND_DIV_NASECOND + na_seconds;
        ns /= div_;
        res.seconds = ns / SECOND_DIV_NASECOND;

        res.na_seconds = ns % SECOND_DIV_NASECOND;
        return res;
    }
    void printf_value() const { qDebug() << "second = " << seconds << " nasecond " << na_seconds << endl; }
};

struct RespData {
    unsigned short sync_seq_id;
    int req_device_id;
    Nanosecond req_access_time;
};

struct PTPTimeInf {
    Nanosecond t1;
    Nanosecond t2;
    Nanosecond t3;
    Nanosecond t4;
    Nanosecond offset;  //从时钟相对主时钟的偏移
    Nanosecond delay;
    void reset() {
        t1.reset();
        t2.reset();
        t3.reset();
        t4.reset();
        offset.reset();
        delay.reset();
    }

    Nanosecond get_offset() {
        //        t1 + delay = t2 + offset;
        //        t3 + offset + delay = t4;
        // delay = t2 + offset - t1;
        // offset = t4-t3- t2 - offset + t1
        offset = ((t4 - t3) - (t2 - t1)) / 2;
        return offset;
    }

    bool ready() {
        if (t2.seconds == 0 || t1.seconds == 0 || t3.seconds == 0 || t4.seconds == 0) {
            return false;
        } else {
            return true;
        }
    }
};

class Ptp : public QObject {
    Q_OBJECT
  public:
    Ptp();
    void start();
    void write_data_test(char* header, int value);
    QNetworkInterface get_bind_network_interface(QString ip_prefix);
    void parse_general_buffer(const QByteArray& data, const QString ip);
    void parse_event_buffer(const QByteArray& data, const QString ip);

    Nanosecond get_cur_nanosecond();
    void create_announce(char* header, int my_id, unsigned int sequence_id);
    void create_sync(char* header, int my_id, unsigned int sequence_id);
    void create_follow_up(char* header, int my_id, unsigned int sequence_id, const Nanosecond& send_time);
    void create_delay_req(char* header, int my_id, unsigned int sequence_id);
    void create_delay_resp(char* header, int my_id, unsigned int sequence_id, int req_id,
                           const Nanosecond& receive_time);

    void send_announce();
    void send_sync_follow();

    void send_req_delay(int delay_req_seq_id);
    void send_resp_delay();
    void set_adjust_time(Nanosecond offset);

    void write_log(QString log);
    void check_ready_sync_seq();
  public slots:
    void receive_general_msg();
    void receive_event_msg();
    void announce_timeout();
    void sync_timeout();
    void delay_resp_timeout();

  private:
    QString bind_ip_prefix_;
    int device_ip_;  //内网ip最后一位
    int domain_number_;
    int clock_id_;
    int device_id_;
    bool is_master_;

    unsigned short announce_seq_id_;
    unsigned short sync_seq_id_;

    QMap<int, PTPTimeInf> sync_id_ptp_time_inf_;
    QUdpSocket* ptp_general_udp_;
    QUdpSocket* ptp_event_udp_;
    QTimer* announce_timer_;
    QTimer* sync_timer_;
    QTimer* delay_resp_timer_;
    QList<RespData> delay_response_inf_;
    QFile log_file_;
};

#endif  // PTP_H
