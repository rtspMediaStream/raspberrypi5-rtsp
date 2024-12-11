#include "Protos.h"
#include "utils.h"
#include "TCPHandler.h"
#include "UDPHandler.h"
#include "MediaStreamHandler.h"
#include "AudioCapture.h"
#include "VideoCapture.h"
#include "OpusEncoder.h"
#include "H264Encoder.h"
#include "global.h"
#include "rtp_header.hpp"
#include "rtp_packet.hpp"
#include "RTPHeader.h"

#include <iostream>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <utility>
#include <random>
#include <algorithm>

MediaStreamHandler::MediaStreamHandler(): streamState(MediaStreamState::eMediaStream_Init){}

void MediaStreamHandler::SendFragmentedRTPPackets(unsigned char* payload, size_t payloadSize, RtpPacket& rtpPacket, const uint32_t timeStamp) {
    unsigned char nalHeader = payload[0]; // NAL 헤더 (첫 바이트)

    if (payloadSize <= MAX_RTP_DATA_SIZE) {
        // 마커 비트 설정
        rtpPacket.get_header().set_marker(1); // 단일 RTP 패킷이므로 마커 비트 활성화

        // 패킷 크기가 MTU 이하인 경우, 단일 RTP 패킷 전송
        memcpy(rtpPacket.get_payload(), payload, payloadSize); // NAL 데이터 복사

        rtpPacket.get_header().set_timestamp(timeStamp);
        rtpPacket.rtp_sendto(udpHandler->GetRTPSocket(), MAX_RTP_PACKET_LEN, 0, (struct sockaddr*)(&udpHandler->GetRTPAddr()));

        return;
    }

    const int64_t packetNum = payloadSize / MAX_RTP_DATA_SIZE;
    const int64_t remainPacketSize = payloadSize % MAX_RTP_DATA_SIZE;
    int64_t pos = 1;    // NAL 헤더(첫 바이트)는 별도 처리

    // 패킷 크기가 MTU를 초과하는 경우, FU-A로 분할
    for (int i = 0; i < packetNum; i++) {
        rtpPacket.get_payload()[0] = (nalHeader & NALU_F_NRI_MASK) | SET_FU_A_MASK;
        rtpPacket.get_payload()[1] = nalHeader & NALU_TYPE_MASK;
        rtpPacket.get_header().set_marker(0);

        if (i == 0)
        {
            // 첫 번째 조각: FU-A Start
            rtpPacket.get_payload()[1] = (nalHeader & NALU_TYPE_MASK) | FU_S_MASK;
        }
        else if (i == packetNum - 1 && remainPacketSize == 0)
        {
            // 마지막 조각: FU-A End
            rtpPacket.get_payload()[1] = (nalHeader & NALU_TYPE_MASK) | FU_E_MASK;
        }
        else
        {
            // 중간 조각: FU-A Middle
            rtpPacket.get_payload()[1] = (nalHeader & NALU_TYPE_MASK);
        }

        // RTP 패킷 생성
        memcpy(rtpPacket.get_payload() + FU_SIZE, &payload[pos], MAX_RTP_DATA_SIZE); // 분할된 데이터 복사
        rtpPacket.rtp_sendto(udpHandler->GetRTPSocket(), MAX_RTP_PACKET_LEN, 0, (struct sockaddr*)(&udpHandler->GetRTPAddr()));

        pos += MAX_RTP_DATA_SIZE;
    }
    if(remainPacketSize > 0) {
        rtpPacket.get_payload()[0] = (nalHeader & NALU_F_NRI_MASK) | SET_FU_A_MASK;
        rtpPacket.get_payload()[1]= (nalHeader & NALU_TYPE_MASK) | FU_E_MASK;
        
        rtpPacket.get_header().set_marker(1);
        // RTP 패킷 생성
        memcpy(rtpPacket.get_payload() + FU_SIZE, &payload[pos], remainPacketSize); // 분할된 데이터 복사
        rtpPacket.rtp_sendto(udpHandler->GetRTPSocket(), RTP_HEADER_SIZE + FU_SIZE + remainPacketSize, 0, (struct sockaddr *)(&udpHandler->GetRTPAddr()));
    }
}

void MediaStreamHandler::HandleMediaStream() {
    Protos protos;

    unsigned int octetCount = 0;
    unsigned int packetCount = 0;
    uint16_t seqNum = (uint16_t)utils::GetRanNum(16);
    unsigned int timestamp = (unsigned int)utils::GetRanNum(16);

    Protos::SenderReport sr;
    int ssrcNum = 0;

    H264Encoder *h264_file = nullptr;

    if(ServerStream::getInstance().type == Audio) {
        std::cout<< "audio" << std::endl;
    }else if(ServerStream::getInstance().type == Video){
        std::cout<< "video file open : " << g_inputFile.c_str() << std::endl;
        h264_file = new H264Encoder(g_inputFile.c_str());
    }


    // RTP 헤더 생성
    RtpHeader rtpHeader(0, 0, ssrcNum);
    if(ServerStream::getInstance().type == Audio)
        rtpHeader.set_payloadType(PROTO_OPUS);
    else if(ServerStream::getInstance().type == Video)
        rtpHeader.set_payloadType(PROTO_H264);
    rtpHeader.set_seq(seqNum);
    rtpHeader.set_timestamp(timestamp);

    // RTP 패킷 생성
    RtpPacket rtpPack{rtpHeader};

    // 파일에서 VideoCapture Queue로 던지기
    std::thread([h264_file]() -> void
                {
    constexpr int64_t target_frame_duration_us = 1000000 / 30; // 30 fps -> 33,333 microseconds per frame

    while (true) {
        auto frame_start_time = std::chrono::high_resolution_clock::now();

        // Get the next frame
        std::pair<const uint8_t *, int64_t> cur_frame = h264_file->get_next_frame();
        const auto ptr_cur_frame = cur_frame.first;
        const auto cur_frame_size = cur_frame.second;

        if (ptr_cur_frame == nullptr) {
            return;
        }

        // Process the frame
        VideoCapture::getInstance().pushImg((unsigned char *)ptr_cur_frame, cur_frame_size);

        // Calculate elapsed time
        auto frame_end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time_us = std::chrono::duration_cast<std::chrono::microseconds>(frame_end_time - frame_start_time).count();

        // Calculate remaining time to wait
        int64_t remaining_time_us = target_frame_duration_us - elapsed_time_us;
        if (remaining_time_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(remaining_time_us));
        }
    } }).detach();

    while (true) {
        if(streamState == MediaStreamState::eMediaStream_Play) { 
            if(ServerStream::getInstance().type == Audio) {
                std::thread([]()->void {
                    short pcmBuffer[OPUS_FRAME_SIZE * OPUS_CHANNELS];
                    OpusEncoder opusEncoder;
                    while(1){
                        unsigned char *encodedBuffer = new unsigned char[MAX_PACKET_SIZE];
                        int rc = AudioCapture::getInstance().read(pcmBuffer, OPUS_FRAME_SIZE);
                        if (rc != OPUS_FRAME_SIZE)
                        {
                            std::cout << "occur audio packet skip." << std::endl;
                            continue;
                        }
                        
                        int bufferSize = opusEncoder.encode(pcmBuffer, OPUS_FRAME_SIZE, encodedBuffer);
                        if (bufferSize < 0)
                        {
                            std::cerr << "Opus encoding error: " << bufferSize << std::endl;
                            delete[] encodedBuffer;
                            continue;
                        }

                        AudioCapture::getInstance().pushData(encodedBuffer, bufferSize);
                    }
                    return ;
                } ).detach();

                unsigned short seq_num = 0;
                unsigned int ssrc = 0;
                while (1)
                {
                    while ( !AudioCapture::getInstance().isBufferEmpty())
                    {
                        // RTPHeader::rtp_header header;
                        // RTPHeader::create(header, seq_num, timestamp, ssrc);

                        // std::pair<const unsigned char *, int> frame = AudioCapture::getInstance().popData();

                        // unsigned char packet[1500];
                        // memcpy(packet, &header, sizeof(header));
                        // memcpy(packet + sizeof(header), frame.first, frame.second);

                        // int packet_size = sizeof(header) + frame.second;
                        // if (sendto(udpHandler->GetRTPSocket(), packet, packet_size, 0, (struct sockaddr *)&udpHandler->GetRTPAddr(), sizeof(udpHandler->GetRTPAddr())) < 0)
                        // {
                        //     throw std::runtime_error("RTP 패킷 전송 오류");
                        // }

                        // make RTP Packet.
                        rtpPack.get_header().set_timestamp(timestamp);
                        rtpPack.get_header().set_marker(true);
                        // if(seq_num %100 == 0){
                        //     rtpPack.get_header().set_marker(true);
                        // }else{
                        //     rtpPack.get_header().set_marker(false);
                        // }

                        std::pair<const uint8_t *, int64_t> frame = AudioCapture::getInstance().popData();
                        memcpy(rtpPack.get_payload(), frame.first, frame.second);
                        rtpPack.rtp_sendto(udpHandler->GetRTPSocket(), RTP_HEADER_SIZE + frame.second, 0, (struct sockaddr *)(&udpHandler->GetRTPAddr()));
                        delete(frame.first);

                        seq_num++;
                        timestamp += OPUS_FRAME_SIZE;
                        packetCount++;
                        octetCount += frame.second;

                        // if (packetCount % 100 == 0)
                        // {
                        //     std::cout << "RTCP sent" << std::endl;
                        //     protos.CreateSR(&sr, timestamp, packetCount, octetCount, PROTO_OPUS);
                        //     udpHandler->SendSenderReport(&sr, sizeof(sr));
                        // }
                    }
                }

            }
            else if (ServerStream::getInstance().type == Video) {
                while (!VideoCapture::getInstance().isEmptyBuffer())
                {
                    std::pair<const uint8_t *, int64_t> cur_frame = VideoCapture::getInstance().popImg();
                    const auto ptr_cur_frame = cur_frame.first;
                    const auto cur_frame_size = cur_frame.second;
                    if (ptr_cur_frame == nullptr || cur_frame_size <= 0)
                    {
                        std::cout << "Not Ready\n";
                        continue;
                    }
                    // RTP 패킷 전송 (FU-A 분할 포함)
                    const int64_t start_code_len = H264Encoder::is_start_code(ptr_cur_frame, cur_frame_size, 4) ? 4 : 3;
                    SendFragmentedRTPPackets((unsigned char *)ptr_cur_frame + start_code_len, cur_frame_size - start_code_len, rtpPack, timestamp);

                    // 주기적으로 RTCP Sender Report 전송
                    packetCount++;
                    octetCount += cur_frame_size;
                    timestamp += 3000; // 90 * 30	== 2700

                    // if (packetCount % 100 == 0)
                    // {
                    //     std::cout << "RTCP sent" << std::endl;
                    //     protos.CreateSR(&sr, timestamp, packetCount, octetCount, PROTO_H264);
                    //     udpHandler->SendSenderReport(&sr, sizeof(sr));
                    // }
                }
            }
        }else if(streamState == MediaStreamState::eMediaStream_Pause) {
            std::unique_lock<std::mutex> lck(streamMutex);
            condition.wait(lck);
        }
        else if (streamState == MediaStreamState::eMediaStream_Teardown) {
            break;
        }
    }
}

void MediaStreamHandler::SetCmd(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(streamMutex);
    if (cmd == "PLAY") {
        streamState = MediaStreamState::eMediaStream_Play;
        condition.notify_all();
    } else if (cmd == "PAUSE") {
        streamState = MediaStreamState::eMediaStream_Pause;
    } else if (cmd == "TEARDOWN") {
        streamState = MediaStreamState::eMediaStream_Teardown;
    }
}