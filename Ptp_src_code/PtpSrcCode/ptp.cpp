#include "ptp.h"

#include <QDateTime>
#include <QDebug>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <chrono>

#include "Bits.h"

Ptp::Ptp() :
    bind_ip_prefix_("192.168.105"), device_ip_(40), domain_number_(1), clock_id_(0), device_id_(0), is_master_(true),
    announce_seq_id_(0), sync_seq_id_(0), ptp_general_udp_(nullptr), ptp_event_udp_(nullptr), announce_timer_(nullptr),
    sync_timer_(nullptr), delay_resp_timer_(nullptr), log_file_("log.txt") {
    bit_init();
    char data[4];
    write_data_test(data, 0x12345678);
    for (int i = 0; i != 4; ++i) {
        printf("write data test i = %x\n", data[i]);
    }

    if (!log_file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        printf("open file error !! \n");
    }
    if (is_big_endian) {
        write_log("pc is big endian");
    } else {
        write_log("pc is small endian");
    }
}

void Ptp::start() {
    get_cur_nanosecond();

    ptp_general_udp_ = new QUdpSocket(this);
    ptp_general_udp_->setSocketOption(QAbstractSocket::MulticastTtlOption, 32);
    //加入组播之前，必须先绑定端口，端口为多播组统一的一个端口。
    bool res = ptp_general_udp_->bind(QHostAddress::AnyIPv4, PTP_GENERAL_PORT,
                                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    QNetworkInterface join_interface;
    if (res) {
        qDebug() << "ptp_general_udp_ bind_port_ " << PTP_GENERAL_PORT << "success" << endl;

        join_interface = get_bind_network_interface(bind_ip_prefix_);
        ptp_general_udp_->setMulticastInterface(join_interface);
        ptp_general_udp_->joinMulticastGroup(QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), join_interface);
        connect(ptp_general_udp_, &QUdpSocket::readyRead, this, &Ptp::receive_general_msg);
    }

    ptp_event_udp_ = new QUdpSocket(this);
    ptp_event_udp_->setSocketOption(QAbstractSocket::MulticastTtlOption, 32);
    //加入组播之前，必须先绑定端口，端口为多播组统一的一个端口。
    res = ptp_event_udp_->bind(QHostAddress::AnyIPv4, PTP_EVENT_PORT,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (res) {
        qDebug() << "ptp_event_udp_ bind_port_ " << PTP_EVENT_PORT << "success" << endl;

        ptp_event_udp_->setMulticastInterface(join_interface);
        ptp_event_udp_->joinMulticastGroup(QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), join_interface);
        connect(ptp_event_udp_, &QUdpSocket::readyRead, this, &Ptp::receive_event_msg);
    }

    announce_timer_ = new QTimer(this);
    connect(announce_timer_, &QTimer::timeout, this, &Ptp::announce_timeout);
    //    announce_timer_->setInterval(1000);
    announce_timer_->setTimerType(Qt::PreciseTimer);
    announce_timer_->start(1000);

    sync_timer_ = new QTimer(this);
    connect(sync_timer_, &QTimer::timeout, this, &Ptp::sync_timeout);
    announce_timer_->setTimerType(Qt::PreciseTimer);
    sync_timer_->setInterval(1000);

    delay_resp_timer_ = new QTimer(this);
    connect(delay_resp_timer_, &QTimer::timeout, this, &Ptp::delay_resp_timeout);
    delay_resp_timer_->setTimerType(Qt::PreciseTimer);
    delay_resp_timer_->setInterval(1000);
    //首次加入时，宣告自己的主时钟信息，和原来的做对比
    send_announce();
}

void Ptp::write_data_test(char* header, int value) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 4;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 4);
    bits_write(&bitsBuffer, 32, value);
}

QNetworkInterface Ptp::get_bind_network_interface(QString ip_prefix) {
    QNetworkInterface join_interface;
    QList<QNetworkInterface> all_networks = QNetworkInterface::allInterfaces();
    for (int i = 0; i != all_networks.size(); ++i) {
        QNetworkInterface inetwork = all_networks.at(i);
        QList<QNetworkAddressEntry> all_address_entrys = inetwork.addressEntries();
        bool ok = false;
        for (int j = 0; j != all_address_entrys.size(); ++j) {
            QNetworkAddressEntry entry = all_address_entrys[j];
            join_interface = inetwork;
            QString ip = entry.ip().toString();
            if (ip.contains(ip_prefix)) {
                ok = true;
                device_ip_ = ip.mid(ip_prefix.size() + 1, ip.size() - ip_prefix.size() - 1).toInt();
                clock_id_ = device_ip_;
                device_id_ = device_ip_;
                qDebug() << "device_ip_ = " << device_ip_ << endl;
                qDebug() << "clock_id_ = " << clock_id_ << endl;
                qDebug() << "device_id_ = " << device_id_ << endl;
                break;
            }
        }
        if (ok) {
            break;
        }
    }
    return join_interface;
}

void Ptp::parse_general_buffer(const QByteArray& data, const QString ip) {
    const char* data_ptr = data.data();
    char domian_num = 0;
    memcpy(&domian_num, data_ptr + 4, 1);
    if (domain_number_ != (int)domian_num) {
        return;
    }
    unsigned char message_type = data_ptr[0] & 0x0F;
    qDebug() << "message_type = " << message_type << endl;
    if (message_type == PTP_ANNOUNCE) {
        int clock_quality = 0;
        if (!is_big_endian) {
            memcpy(&((char*)&clock_quality)[3], data_ptr + 48, 1);
            memcpy(&((char*)&clock_quality)[2], data_ptr + 49, 1);
            memcpy(&((char*)&clock_quality)[1], data_ptr + 50, 1);
            memcpy(&((char*)&clock_quality)[0], data_ptr + 51, 1);
        } else {
            memcpy((char*)&clock_quality, data_ptr + 48, 4);
        }
        qDebug() << "clock_quality = " << clock_quality << endl;
        if (clock_quality > clock_id_) {
            is_master_ = false;
            if (sync_timer_->isActive()) {
                sync_timer_->stop();

                sync_id_ptp_time_inf_.clear();
                delay_response_inf_.clear();
                sync_seq_id_ = 0;
                // timer启动需要1秒时间，如果是新加入的设备加入时，原来是有主时钟的，如果新加入的设备id最大，那么将会更换主时钟
                //原来的主时钟会发送announce，当前加入的也会发送，两者竞争必有一个是失败的。用ip当作clockid_作为唯一是可以保证这个结果
            }
            if (delay_resp_timer_->isActive()) {
                delay_resp_timer_->stop();
            }
        } else {
            //开始执行timer,发送sync
            is_master_ = true;
            if (!sync_timer_->isActive()) {
                sync_timer_->start();
                sync_id_ptp_time_inf_.clear();
                delay_response_inf_.clear();
                sync_seq_id_ = 0;
                // timer启动需要1秒时间，如果是新加入的设备加入时，原来是有主时钟的，如果新加入的设备id最大，那么将会更换主时钟
                //原来的主时钟会发送announce，当前加入的也会发送，两者竞争必有一个是失败的。用ip当作clockid_作为唯一是可以保证这个结果
            }
            if (!delay_resp_timer_->isActive()) {
                delay_resp_timer_->start();
            }
        }
    } else if (message_type == PTP_FOLLOW_UP && !is_master_) {
        Nanosecond t1;
        t1.seconds = 0;
        if (!is_big_endian) {
            memcpy(&((char*)&t1.seconds)[5], data_ptr + 34, 1);
            memcpy(&((char*)&t1.seconds)[4], data_ptr + 35, 1);
            memcpy(&((char*)&t1.seconds)[3], data_ptr + 36, 1);
            memcpy(&((char*)&t1.seconds)[2], data_ptr + 37, 1);
            memcpy(&((char*)&t1.seconds)[1], data_ptr + 38, 1);
            memcpy(&((char*)&t1.seconds)[0], data_ptr + 39, 1);

            memcpy(&((char*)&t1.na_seconds)[3], data_ptr + 40, 1);
            memcpy(&((char*)&t1.na_seconds)[2], data_ptr + 41, 1);
            memcpy(&((char*)&t1.na_seconds)[1], data_ptr + 42, 1);
            memcpy(&((char*)&t1.na_seconds)[0], data_ptr + 43, 1);
        } else {
            memcpy((char*)&t1.na_seconds + 2, data_ptr + 34, 6);
            memcpy((char*)&t1.na_seconds, data_ptr + 40, 4);
        }

        qDebug() << "receive PTP_FOLLOW_UP t1 seconds = " << t1.seconds << " t1.na_seconds" << t1.na_seconds << endl;
        unsigned short req_delay_seq_id;
        if (!is_big_endian) {
            for (int i = 0; i != 2; ++i) {
                memcpy(&((char*)&req_delay_seq_id)[1 - i], data_ptr + 30 + i, 1);
            }
        } else {
            memcpy((char*)&req_delay_seq_id, data_ptr + 30, 2);
        }

        if (sync_id_ptp_time_inf_.contains(req_delay_seq_id)) {
            sync_id_ptp_time_inf_[req_delay_seq_id].t1 = t1;
        } else {
            PTPTimeInf t;
            t.reset();
            t.t1 = t1;
            sync_id_ptp_time_inf_[req_delay_seq_id] = t;
        }
        send_req_delay(req_delay_seq_id);
    } else if (message_type == PTP_DELAY_RESP && !is_master_) {
        int req_id = 0;
        if (!is_big_endian) {
            for (int i = 0; i != 4; ++i) {
                memcpy(&((char*)&req_id)[3 - i], data_ptr + 50 + i, 1);
            }
        } else {
            memcpy((char*)&req_id, data_ptr + 50, 4);
        }
        qDebug() << "resp req device id = " << req_id << endl;
        if (req_id == device_id_) {
            Nanosecond t4;
            t4.seconds = 0;
            if (!is_big_endian) {
                memcpy(&((char*)&t4.seconds)[5], data_ptr + 34, 1);
                memcpy(&((char*)&t4.seconds)[4], data_ptr + 35, 1);
                memcpy(&((char*)&t4.seconds)[3], data_ptr + 36, 1);
                memcpy(&((char*)&t4.seconds)[2], data_ptr + 37, 1);
                memcpy(&((char*)&t4.seconds)[1], data_ptr + 38, 1);
                memcpy(&((char*)&t4.seconds)[0], data_ptr + 39, 1);

                memcpy(&((char*)&t4.na_seconds)[3], data_ptr + 40, 1);
                memcpy(&((char*)&t4.na_seconds)[2], data_ptr + 41, 1);
                memcpy(&((char*)&t4.na_seconds)[1], data_ptr + 42, 1);
                memcpy(&((char*)&t4.na_seconds)[0], data_ptr + 43, 1);
            } else {
                memcpy((char*)&t4.na_seconds + 2, data_ptr + 34, 6);
                memcpy((char*)&t4.na_seconds, data_ptr + 40, 4);
            }

            unsigned short sync_resp_id;
            if (!is_big_endian) {
                for (int i = 0; i != 2; ++i) {
                    memcpy(&((char*)&sync_resp_id)[1 - i], data_ptr + 30 + i, 1);
                }
            } else {
                memcpy((char*)&sync_resp_id, data_ptr + 30, 2);
            }

            if (sync_id_ptp_time_inf_.contains(sync_resp_id)) {
                sync_id_ptp_time_inf_[sync_resp_id].t4 = t4;
            } else {
                PTPTimeInf t;
                t.reset();
                t.t4 = t4;
                sync_id_ptp_time_inf_[sync_resp_id] = t;
            }
            qDebug() << "resp PTP_DELAY_RESP time t4.seconds = " << t4.seconds << ",na second " << t4.na_seconds
                     << endl;
            check_ready_sync_seq();
        }
    }
}

void Ptp::parse_event_buffer(const QByteArray& data, const QString ip) {
    const char* data_ptr = data.data();
    char domian_num = 0;
    memcpy(&domian_num, data_ptr + 4, 1);
    if (domain_number_ != (int)domian_num) {
        return;
    }
    unsigned char message_type = data_ptr[0] & 0x0F;
    qDebug() << "message_type = " << message_type << endl;

    if (message_type == PTP_SYNC && !is_master_) {
        unsigned short sync_seq_id;
        if (!is_big_endian) {
            for (int i = 0; i != 2; ++i) {
                memcpy(&((char*)&sync_seq_id)[1 - i], data_ptr + 30 + i, 1);
            }
        } else {
            memcpy((char*)&sync_seq_id, data_ptr + 30, 2);
        }

        Nanosecond t2 = get_cur_nanosecond();
        if (sync_id_ptp_time_inf_.contains(sync_seq_id)) {
            sync_id_ptp_time_inf_[sync_seq_id].t2 = t2;
        } else {
            PTPTimeInf t;
            t.reset();
            t.t2 = t2;
            sync_id_ptp_time_inf_[sync_seq_id] = t;
        }

        QString log_str = QString("send req_delay time t2 ,s = %1 ,ns= %2").arg(t2.seconds).arg(t2.na_seconds);
        write_log(log_str);
        qDebug() << "receive PTP_SYNC t2 seconds = " << t2.seconds << " t2.na_seconds" << t2.na_seconds << endl;
    } else if (message_type == PTP_DELAY_REQ && is_master_) {
        //解析数据存下来
        Nanosecond t4 = get_cur_nanosecond();
        int req_id = 0;
        if (!is_big_endian) {
            for (int i = 0; i != 4; ++i) {
                memcpy(&((char*)&req_id)[3 - i], data_ptr + 26 + i, 1);
            }
        } else {
            memcpy((char*)&req_id, data_ptr + 26, 4);
        }
        qDebug() << "receive PTP_DELAY_REQ t4 seconds = " << t4.seconds << " t4.na_seconds" << t4.na_seconds << endl;
        qDebug() << "req device id = " << req_id << endl;
        RespData resp;
        unsigned short delay_req_seq_id;
        if (!is_big_endian) {
            for (int i = 0; i != 2; ++i) {
                memcpy(&((char*)&delay_req_seq_id)[1 - i], data_ptr + 30 + i, 1);
            }
        } else {
            memcpy((char*)&delay_req_seq_id, data_ptr + 30, 2);
        }
        resp.req_device_id = req_id;
        resp.req_access_time = t4;
        resp.sync_seq_id = delay_req_seq_id;
        delay_response_inf_.append(resp);
    }
}

Nanosecond Ptp::get_cur_nanosecond() {
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    Nanosecond nano_second;
    std::chrono::nanoseconds ns;
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) % SECOND_DIV_NASECOND;
    nano_second.na_seconds = ns.count();
    nano_second.seconds = now_time_t;
    return nano_second;
}

void Ptp::create_announce(char* header, int my_id, unsigned int sequence_id) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 64;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 64);

    bits_write(&bitsBuffer, 4, 0);             // 0-3bit messageType,
    bits_write(&bitsBuffer, 4, PTP_ANNOUNCE);  // 4-7bit是transportSpecific

    bits_write(&bitsBuffer, 4, 0);  // 0-3bit versionPtp,
    bits_write(&bitsBuffer, 4, 2);  // 4-7bit是reserved

    bits_write(&bitsBuffer, 16, 64);  //报文长度

    bits_write(&bitsBuffer, 8, domain_number_);  //域号
    bits_write(&bitsBuffer, 8, 0);               // reserved
    bits_write(&bitsBuffer, 16, 0);              // flagField
    bits_write(&bitsBuffer, 64, 0);              //修正域
    bits_write(&bitsBuffer, 32, 0);              // reserved[4]

    bits_write(&bitsBuffer, 80, my_id);
    // 设备唯一id，相当于身份，每个域中的每个参与同步的设备id唯一不重复，相当于用户id

    bits_write(&bitsBuffer, 16, sequence_id);  //序号
    bits_write(&bitsBuffer, 8, 5);             // announce的controlField=5.other
    bits_write(&bitsBuffer, 8, 1);             //消息间隔1s一次

    //下面是时钟信息

    auto cur_time = get_cur_nanosecond();

    char* second_ptr = (char*)&cur_time.seconds;
    if (!is_big_endian) {
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[1]);
        bits_write(&bitsBuffer, 8, second_ptr[0]);
    } else {
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[6]);
        bits_write(&bitsBuffer, 8, second_ptr[7]);
    }
    bits_write(&bitsBuffer, 32, cur_time.na_seconds);
    //  34	10	Origin Timestamp	数值为 0 或精度为 ±1 ns 的时间戳

    bits_write(&bitsBuffer, 16, 0);
    //  44	2	CurrentUtcOffset	UTC 与 TAI 时间标尺间的闰秒时间差

    bits_write(&bitsBuffer, 8, 0);
    //  46	1	Reserved

    bits_write(&bitsBuffer, 8, 96);
    //  47	1	GrandmasterPriority1	用户定义的 grandmaster 优先级

    bits_write(&bitsBuffer, 32, clock_id_);
    //  48	4	GrandmasterClockQuality	grandmaster 的时间质量级别

    bits_write(&bitsBuffer, 8, 91);
    //  52	1	GrandmasterPriority2

    bits_write(&bitsBuffer, 64, clock_id_);
    //  53	8	GrandmasterIdentity	grandmaster 的时钟设备 ID

    bits_write(&bitsBuffer, 16, 0);
    //  61	2	StepRemoved	grandmaster 与 Slave 设备间的时钟路径跳数

    bits_write(&bitsBuffer, 8, 0);
    //  63	1	TimeSource	时间源头类型：
    //  GPS - GPS 卫星传送时钟
    //  PTP - PTP 时钟
    //  NTF - NTP 时钟
    //  Hand_set - 人工调整校准时钟
}

void Ptp::create_sync(char* header, int my_id, unsigned int sequence_id) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 44;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 44);

    bits_write(&bitsBuffer, 4, 0);         // 0-3bit messageType,
    bits_write(&bitsBuffer, 4, PTP_SYNC);  // 4-7bit是transportSpecific

    bits_write(&bitsBuffer, 4, 0);  // 0-3bit versionPtp,
    bits_write(&bitsBuffer, 4, 2);  // 4-7bit是reserved

    bits_write(&bitsBuffer, 16, 44);  //报文长度

    bits_write(&bitsBuffer, 8, domain_number_);  //域号
    bits_write(&bitsBuffer, 8, 0);               // reserved
    bits_write(&bitsBuffer, 16, 0);              // flagField
    bits_write(&bitsBuffer, 64, 0);              //修正域
    bits_write(&bitsBuffer, 32, 0);              // reserved[4]

    bits_write(&bitsBuffer, 48, 0);
    bits_write(&bitsBuffer, 32, my_id);  // //发送方id

    // 设备唯一id，相当于身份，每个域中的每个参与同步的设备id唯一不重复，相当于用户id

    bits_write(&bitsBuffer, 16, sequence_id);  //序号
    bits_write(&bitsBuffer, 8, 0);             // sync的controlField=0
    bits_write(&bitsBuffer, 8, 1);             //消息间隔1s一次
    Nanosecond data_init_time = get_cur_nanosecond();

    // second 是8个字节，实际存储是6个字节，且是小端模式，所以要去掉头部两个字节
    char* second_ptr = (char*)&data_init_time.seconds;

    if (!is_big_endian) {
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[1]);
        bits_write(&bitsBuffer, 8, second_ptr[0]);
    } else {
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[6]);
        bits_write(&bitsBuffer, 8, second_ptr[7]);
    }

    bits_write(&bitsBuffer, 32, data_init_time.na_seconds);
}
void Ptp::create_follow_up(char* header, int my_id, unsigned int sequence_id, const Nanosecond& send_time) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 44;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 44);

    bits_write(&bitsBuffer, 4, 0);              // 0-3bit messageType,
    bits_write(&bitsBuffer, 4, PTP_FOLLOW_UP);  // 4-7bit是transportSpecific

    bits_write(&bitsBuffer, 4, 0);  // 0-3bit versionPtp,
    bits_write(&bitsBuffer, 4, 2);  // 4-7bit是reserved

    bits_write(&bitsBuffer, 16, 44);  //报文长度

    bits_write(&bitsBuffer, 8, domain_number_);  //域号
    bits_write(&bitsBuffer, 8, 0);               // reserved
    bits_write(&bitsBuffer, 16, 0x0200);         // flagField
    bits_write(&bitsBuffer, 64, 0);              //修正域
    bits_write(&bitsBuffer, 32, 0);              // reserved[4]

    bits_write(&bitsBuffer, 48, 0);
    bits_write(&bitsBuffer, 32, my_id);  // //发送方id

    bits_write(&bitsBuffer, 16, sequence_id);  //序号
    bits_write(&bitsBuffer, 8, 2);             // follow up 的controlField=2
    bits_write(&bitsBuffer, 8, 1);             //消息间隔1s一次

    char* second_ptr = (char*)&send_time.seconds;

    if (!is_big_endian) {
        for (int i = 0; i != 8; ++i) {
            QString wirte_value =
                QString("write follow up t1, 34 + i = %1").arg(second_ptr[7 - i] & 0xFF, 2, 16, QChar('0'));
            qDebug() << wirte_value << endl;
        }
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[1]);
        bits_write(&bitsBuffer, 8, second_ptr[0]);
    } else {
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[6]);
        bits_write(&bitsBuffer, 8, second_ptr[7]);
    }

    bits_write(&bitsBuffer, 32, send_time.na_seconds);
    qDebug() << "**********create_follow_up***********" << endl;
    send_time.printf_value();
}
void Ptp::create_delay_req(char* header, int my_id, unsigned int sequence_id) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 44;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 44);

    bits_write(&bitsBuffer, 4, 0);              // 0-3bit messageType,
    bits_write(&bitsBuffer, 4, PTP_DELAY_REQ);  // 4-7bit是transportSpecific

    bits_write(&bitsBuffer, 4, 0);  // 0-3bit versionPtp,
    bits_write(&bitsBuffer, 4, 2);  // 4-7bit是reserved

    bits_write(&bitsBuffer, 16, 44);  //报文长度

    bits_write(&bitsBuffer, 8, domain_number_);  //域号
    bits_write(&bitsBuffer, 8, 0);               // reserved
    bits_write(&bitsBuffer, 16, 0x0200);         // flagField //两步
    bits_write(&bitsBuffer, 64, 0);              //修正域
    bits_write(&bitsBuffer, 32, 0);              // reserved[4]

    bits_write(&bitsBuffer, 48, 0);
    bits_write(&bitsBuffer, 32, my_id);  // //发送方id

    bits_write(&bitsBuffer, 16, sequence_id);  //序号
    bits_write(&bitsBuffer, 8, 1);             // delay_req的controlField=1
    bits_write(&bitsBuffer, 8, 1);             //消息间隔1s一次

    Nanosecond data_init_time = get_cur_nanosecond();
    char* second_ptr = (char*)&data_init_time.seconds;
    if (!is_big_endian) {
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[1]);
        bits_write(&bitsBuffer, 8, second_ptr[0]);
    } else {
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[6]);
        bits_write(&bitsBuffer, 8, second_ptr[7]);
    }
    bits_write(&bitsBuffer, 32, data_init_time.na_seconds);

    qDebug() << "******* create_delay_req my_id = " << my_id << endl;
}
void Ptp::create_delay_resp(char* header, int my_id, unsigned int sequence_id, int req_id,
                            const Nanosecond& receive_time) {
    bits_buffer_t bitsBuffer;
    bitsBuffer.i_size = 54;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char*)(header);
    memset(bitsBuffer.p_data, 0, 54);

    bits_write(&bitsBuffer, 4, 0);               // 0-3bit messageType,  delay_resp type =0x09
    bits_write(&bitsBuffer, 4, PTP_DELAY_RESP);  // 4-7bit是transportSpecific

    bits_write(&bitsBuffer, 4, 0);  // 0-3bit versionPtp,
    bits_write(&bitsBuffer, 4, 2);  // 4-7bit是reserved

    bits_write(&bitsBuffer, 16, 54);  //报文长度

    bits_write(&bitsBuffer, 8, domain_number_);  //域号
    bits_write(&bitsBuffer, 8, 0);               // reserved
    bits_write(&bitsBuffer, 16, 0x0200);         // flagField
    bits_write(&bitsBuffer, 64, 0);              //修正域
    bits_write(&bitsBuffer, 32, 0);              // reserved[4]

    bits_write(&bitsBuffer, 48, 0);
    bits_write(&bitsBuffer, 32, my_id);  // //发送方id

    bits_write(&bitsBuffer, 16, sequence_id);  //序号
    bits_write(&bitsBuffer, 8, 3);             // delay_resp的controlField=0x03
    bits_write(&bitsBuffer, 8, 1);             //消息间隔1s一次

    char* second_ptr = (char*)&receive_time.seconds;
    if (!is_big_endian) {
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[1]);
        bits_write(&bitsBuffer, 8, second_ptr[0]);
    } else {
        bits_write(&bitsBuffer, 8, second_ptr[2]);
        bits_write(&bitsBuffer, 8, second_ptr[3]);
        bits_write(&bitsBuffer, 8, second_ptr[4]);
        bits_write(&bitsBuffer, 8, second_ptr[5]);
        bits_write(&bitsBuffer, 8, second_ptr[6]);
        bits_write(&bitsBuffer, 8, second_ptr[7]);
    }
    bits_write(&bitsBuffer, 32, receive_time.na_seconds);

    bits_write(&bitsBuffer, 48, 0);  //响应 Delay_Req 报文的发送设备端口 ID。
    bits_write(&bitsBuffer, 32, req_id);
}
void Ptp::send_announce() {
    qDebug() << "************send_announce***************" << endl;
    char announce_inf[64];
    create_announce(announce_inf, device_id_, announce_seq_id_);
    QByteArray byte_data(announce_inf, 64);
    int send_size =
        ptp_general_udp_->writeDatagram(byte_data, QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), PTP_GENERAL_PORT);
    qDebug() << "send ip address = " << DEFAULT_PTP_DOMAIN_ADDRESS << Qt::endl;
    qDebug() << "send port = " << PTP_GENERAL_PORT << Qt::endl;
    qDebug() << "send size = " << send_size << Qt::endl;
    announce_seq_id_++;
}

void Ptp::send_sync_follow() {
    qDebug() << "************send_sync_follow***************" << endl;
    char sync_inf[44];
    create_sync(sync_inf, device_id_, sync_seq_id_);
    QByteArray byte_data(sync_inf, 44);
    int send_size = ptp_event_udp_->writeDatagram(byte_data, QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), PTP_EVENT_PORT);
    Nanosecond ns = get_cur_nanosecond();

    QString log_str = QString("send sync t1 time ,s = %1 ,ns= %2").arg(ns.seconds).arg(ns.na_seconds);
    write_log(log_str);
    if (send_size == 44) {
        char follow_up_inf[44];
        create_follow_up(follow_up_inf, device_id_, sync_seq_id_, ns);
        //每一次同步，使用同一个id，发送 sync,follow up, delay req,delay resp协议
        QByteArray byte_data_follow(follow_up_inf, 44);
        ptp_general_udp_->writeDatagram(byte_data_follow, QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), PTP_GENERAL_PORT);
    }
    sync_seq_id_++;
}

void Ptp::send_req_delay(int delay_req_seq_id) {
    qDebug() << "************send_req_delay***************" << endl;
    char dealy_req_inf[44];
    create_delay_req(dealy_req_inf, device_id_, delay_req_seq_id);
    QByteArray byte_data(dealy_req_inf, 44);
    int send_size = ptp_event_udp_->writeDatagram(byte_data, QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), PTP_EVENT_PORT);
    Nanosecond t3 = get_cur_nanosecond();

    if (sync_id_ptp_time_inf_.contains(delay_req_seq_id)) {
        sync_id_ptp_time_inf_[delay_req_seq_id].t3 = t3;
    } else {
        PTPTimeInf t;
        t.reset();
        t.t3 = t3;
        sync_id_ptp_time_inf_[delay_req_seq_id] = t;
    }
    QString log_str = QString("send req_delay time T3 ,s = %1 ,ns= %2").arg(t3.seconds).arg(t3.na_seconds);
    write_log(log_str);
    qDebug() << "send PTP_DELAY_REQ t3 seconds = " << t3.seconds << " t3.na_seconds" << t3.na_seconds << endl;
}

void Ptp::send_resp_delay() {
    qDebug() << "************send_resp_delay***************" << endl;
    for (RespData resp : delay_response_inf_) {
        char dealy_resp_inf[54];
        qDebug() << "send_resp_delay req_id = " << resp.req_device_id << " resp.sync_seq_id = " << resp.sync_seq_id
                 << endl;
        QString log_str = QString("send t4 time ,s = %1 ,ns= %2")
                              .arg(resp.req_access_time.seconds)
                              .arg(resp.req_access_time.na_seconds);
        write_log(log_str);
        create_delay_resp(dealy_resp_inf, device_id_, resp.sync_seq_id, resp.req_device_id, resp.req_access_time);
        QByteArray byte_data(dealy_resp_inf, 54);
        int send_size =
            ptp_event_udp_->writeDatagram(byte_data, QHostAddress(DEFAULT_PTP_DOMAIN_ADDRESS), PTP_GENERAL_PORT);
    }
    delay_response_inf_.clear();
}

void Ptp::set_adjust_time(Nanosecond offset) {
    qDebug() << "************set_adjust_time***************" << endl;
    Nanosecond cur = get_cur_nanosecond();
    cur.printf_value();
    Nanosecond adjuest = cur + offset;
    long long offset_naseconds = offset.seconds * SECOND_DIV_NASECOND + offset.na_seconds;

    qDebug() << "**********offset naseconds = " << offset.na_seconds << endl;
    qDebug() << "**********offset second = " << offset.seconds << endl;
    // windows下没有设置纳秒级的接口，一般window始终精度没那么高
    // linux下直接调用

#ifdef Q_OS_LINUX
    timespec time;
    time.tv_nsec = adjuest.na_seconds;
    time.tv_sec = adjuest.seconds;
    clock_settime(CLOCK_REALTIME, &time);

//#elif defined(Q_OS_WIN)
#else
    QDateTime date_adj;
    long long msecond = (adjuest.seconds * SECOND_DIV_NASECOND + adjuest.na_seconds) / 1000000;
    qDebug() << "**********all millseconds = " << msecond << endl;
    date_adj.setMSecsSinceEpoch(msecond);
    SYSTEMTIME stNew;
    stNew.wYear = date_adj.date().year();

    stNew.wMonth = date_adj.date().month();

    stNew.wDay = date_adj.date().day();
    stNew.wHour = date_adj.time().hour();
    stNew.wMinute = date_adj.time().minute();
    stNew.wSecond = date_adj.time().second();
    stNew.wMilliseconds = msecond % 1000;

    qDebug() << "adjust time = " << date_adj.toString("yyyy-MM-dd hh:mm:ss") << endl;
    qDebug() << "stNew wHour = " << stNew.wHour << endl;
    qDebug() << "stNew wMinute = " << stNew.wMinute << endl;
    qDebug() << "stNew wSecond = " << stNew.wSecond << endl;
    qDebug() << "**********stNew.wMilliseconds = " << stNew.wMilliseconds << endl;

    bool res = SetLocalTime(&stNew);
    if (res) {
        qDebug() << "**********adjust time success!!! " << endl;
    } else {
        // windows 因为权限问题，调整一般就是失败的，需要以管理员权限运行qtcreator或者程序
        qDebug() << "**********adjust time failed!!! " << endl;
    }

#endif
}

void Ptp::write_log(QString log) {
    QTextStream out(&log_file_);
    //把数据写到html文件中
    out << log << endl;
    out.flush();
}

void Ptp::check_ready_sync_seq() {
    // 收到一次时间，由于网络可能不稳定，收到t1，t2，和t4的顺序可能不同
    for (int seq_id : sync_id_ptp_time_inf_.keys()) {
        bool ready_ok = sync_id_ptp_time_inf_[seq_id].ready();
        if (ready_ok) {
            Nanosecond offset_time = sync_id_ptp_time_inf_[seq_id].get_offset();
            set_adjust_time(offset_time);
            sync_id_ptp_time_inf_.remove(seq_id);
            break;
        }
    }
}

void Ptp::receive_general_msg() {
    while (ptp_general_udp_->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(ptp_general_udp_->pendingDatagramSize());
        QHostAddress peerAddr;
        quint16 peerPort;
        ptp_general_udp_->readDatagram(data.data(), data.size(), &peerAddr, &peerPort);

        // QString peer = "[From ] +" + peerAddr.toString() + ":" + QString::number(peerPort) + "] ";
        // qDebug() << "receive  data = " << peer << Qt::endl;

        parse_general_buffer(data, peerAddr.toString());
    };
}

void Ptp::receive_event_msg() {
    while (ptp_event_udp_->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(ptp_event_udp_->pendingDatagramSize());
        QHostAddress peerAddr;
        quint16 peerPort;
        ptp_event_udp_->readDatagram(data.data(), data.size(), &peerAddr, &peerPort);

        // QString str = data.data();
        // QString peer = "[From ] +" + peerAddr.toString() + ":" + QString::number(peerPort) + "] ";
        // qDebug() << "receive  data = " << peer << Qt::endl;
        parse_event_buffer(data, peerAddr.toString());
    };
}

void Ptp::announce_timeout() {
    if (is_master_) {
        qDebug() << "is master send announce" << endl;
        send_announce();
    }
}

void Ptp::sync_timeout() {
    if (is_master_) {
        qDebug() << "is master send sync_follow" << endl;
        send_sync_follow();
    }
}

void Ptp::delay_resp_timeout() {
    //定时回复delay_req
    if (is_master_) {
        send_resp_delay();
    }
}

BOOL EnableSetTimePriviledge() {
    HANDLE m_hToken;
    TOKEN_PRIVILEGES m_TokenPriv;
    BOOL m_bTakenPriviledge;

    BOOL bOpenToken = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &m_hToken);

    m_bTakenPriviledge = FALSE;
    if (!bOpenToken) {
        if (GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
            // Must be running on 95 or 98 not NT. In that case just ignore the error
            SetLastError(ERROR_SUCCESS);
            if (!m_hToken)
                CloseHandle(m_hToken);
            return TRUE;
        }

        if (!m_hToken)
            CloseHandle(m_hToken);
        return FALSE;
    }
    ZeroMemory(&m_TokenPriv, sizeof(TOKEN_PRIVILEGES));
    if (!LookupPrivilegeValue(NULL, SE_SYSTEMTIME_NAME, &m_TokenPriv.Privileges[0].Luid)) {
        if (!m_hToken)
            CloseHandle(m_hToken);
        return FALSE;
    }
    m_TokenPriv.PrivilegeCount = 1;
    m_TokenPriv.Privileges[0].Attributes |= SE_PRIVILEGE_ENABLED;
    m_bTakenPriviledge = TRUE;

    BOOL bSuccess = AdjustTokenPrivileges(m_hToken, FALSE, &m_TokenPriv, 0, NULL, 0);

    if (!m_hToken)
        CloseHandle(m_hToken);

    return bSuccess;
}
