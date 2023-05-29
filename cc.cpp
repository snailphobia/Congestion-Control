// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
#include "cc.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
using namespace std;

////////////////////////////////////////////////////////////////
//  CC SOURCE. Aici este codul care ruleaza pe transmitatori. Tot ce avem nevoie pentru a implementa
//  un algoritm de congestion control se gaseste aici.
////////////////////////////////////////////////////////////////

#define _C      0.4
#define _beta   0.2

int CCSrc::_global_node_count = 0;
unsigned long dMin = 0;
static simtime_picosec duration = 0;
static auto cwnd_delta = 0;
static double origin_point = 0.0;
static simtime_picosec estart = 0;
static auto ack_count = 0,
            max_count = 0;
static int delta = 0;
static double w_last = 0.0, w_tcp = 0.0, _K = 0.0; 
static bool friendliness = true,
            fconv = true;
CCSrc::CCSrc(EventList &eventlist)
    : EventSource(eventlist,"cc"), _flow(NULL)
{
    _mss = Packet::data_packet_size();
    _acks_received = 0;
    _nacks_received = 0;

    _highest_sent = 0;
    _next_decision = 0;
    _flow_started = false;
    _sink = 0;
  
    _node_num = _global_node_count++;
    _nodename = "CCsrc " + to_string(_node_num);

    _cwnd = 10 * _mss;
    _ssthresh = 0xFFFFFFFFFF;
    _flightsize = 0;
    _flow._name = _nodename;
    setName(_nodename);
}
static void reset() {
    w_last = 0;
    w_tcp = 0;
    ack_count = 0;
    origin_point = 0.0;
    estart = 0;
    dMin = 0;
    _K = 0.0;
}

/* Porneste transmisia fluxului de octeti */
void CCSrc::startflow(){
    cout << "Start flow " << _flow._name << " at " << timeAsSec(eventlist().now()) << "s" << endl;
    reset();
    _flow_started = true;
    _highest_sent = 0;
    _packets_sent = 0;

    while (_flightsize + _mss < _cwnd)
        send_packet();
}

/* Initializeaza conexiunea la host-ul sink */
void CCSrc::connect(Route* routeout, Route* routeback, CCSink& sink, simtime_picosec starttime) {
    assert(routeout);
    _route = routeout;
    
    _sink = &sink;
    _flow._name = _name;
    _sink->connect(*this, routeback);

    eventlist().sourceIsPending(*this,starttime);
}


static int cubic_update(const CCSrc& self, const simtime_picosec timestamp) {
    ack_count++;
    double _K = 0.0;
    if (estart <= 0) {
        estart = timestamp;
        if (self._cwnd < w_last) {
            _K = cbrt((w_last - self._cwnd) / _C);
            origin_point = w_last;
        } else {
            _K = 0.0;
            origin_point = self._cwnd;
        }
        ack_count = 1;
    }
    auto t = timestamp + dMin - estart;
    auto target = origin_point + _C * pow(t - _K, 3);

    if (target > self._cwnd) 
        delta = self._cwnd / (target - self._cwnd);
    else
        delta = 100 * self._cwnd;

    if (friendliness) {
        w_tcp = w_tcp + 3 * _beta / (2 - _beta) * ((double)ack_count) / self._cwnd;
        ack_count = 0;
        if (w_tcp > self._cwnd) {
            max_count = self._cwnd / (w_tcp - self._cwnd);
            if (delta > max_count) delta = max_count;
        }
    }
    return delta;
}

/* Variabilele cu care vom lucra:
    _nacks_received
    _flightsize -> numarul de bytes aflati in zbor
    _mmm -> maximum segment size
    _next_decision 
    _highest_sent
    _cwnd
    _ssthresh
    
    CCAck._ts -> timestamp ACK
    eventlist.now -> timpul actual
    eventlist.now - CCAck._tx -> latency
    
    ack.ackno();
    
    > Puteti include orice alte variabile in restul codului in functie de nevoie.
*/
/* TODO: In mare parte aici vom avea implementarea algoritmului si in functie de nevoie in celelalte functii */

void CCSrc::processNack(const CCNack& nack){    
    //cout << "CC " << _name << " got NACK " <<  nack.ackno() << _highest_sent << " at " << timeAsMs(eventlist().now()) << " us" << endl;    
    _nacks_received ++;
    _flightsize -= _mss;    
    
    // if (nack.ackno()>=_next_decision) {    
    //     _cwnd = _cwnd / 2;    
    //     if (_cwnd < _mss)    
    //         _cwnd = _mss;    

    //     _ssthresh = _cwnd;
    //     //cout << "CWNDD " << _cwnd/_mss << endl; 
    //     // eventlist.now
    
    //     _next_decision = _highest_sent + _cwnd;    
    // }
    estart = 0;
    if (_cwnd < w_last && fconv) 
        w_last = _cwnd * (2 - _beta) / 2;
    else
        w_last = _cwnd;
    _ssthresh = w_last;
    _cwnd = _cwnd * (1 - _beta);
}

/* Process an ACK.  Mostly just housekeeping*/    
void CCSrc::processAck(const CCAck& ack) {    
    // CCAck::seq_t ackno = ack.ackno();    
    
    _acks_received++;
    _flightsize -= _mss;    
    
    // if (_cwnd < _ssthresh)    
    //     _cwnd += _mss;    
    // else    
    //     _cwnd += _mss * _mss / _cwnd;    
    auto ts = ack.ts();    
    auto rtt = eventlist().now() - ts;

    if (dMin) dMin = min(dMin, rtt);
    else dMin = rtt;

    if (_cwnd < _ssthresh) _cwnd += _mss * abs((double)_ssthresh - _cwnd) / ((double) _ssthresh) * (1 - _C);
    else {
        delta = cubic_update(*this, ts);
        if (cwnd_delta > delta)
            _cwnd += delta * _mss, cwnd_delta = 0;
        else cwnd_delta += 1;
    }
    //cout << "CWNDI " << _cwnd/_mss << endl;    
}    


/* Functia de receptie, in functie de ce primeste cheama processLoss sau processACK */
void CCSrc::receivePacket(Packet& pkt) 
{
    if (!_flow_started){
        return; 
    }

    switch (pkt.type()) {
    case CCNACK: 
        processNack((const CCNack&)pkt);
        pkt.free();
        break;
    case CCACK:
        processAck((const CCAck&)pkt);
        pkt.free();
        break;
    default:
        reset();
        cout << "Got packet with type " << pkt.type() << endl;
        // abort();
    }

    //now send packets!
    while (_flightsize + _mss < _cwnd)
        send_packet();
}

// Note: the data sequence number is the number of Byte1 of the packet, not the last byte.
/* Functia care se este chemata pentru transmisia unui pachet */
void CCSrc::send_packet() {
    CCPacket* p = NULL;

    assert(_flow_started);

    p = CCPacket::newpkt(*_route,_flow, _highest_sent+1, _mss, eventlist().now());
    
    _highest_sent += _mss;
    _packets_sent++;

    _flightsize += _mss;

    //cout << "Sent " << _highest_sent+1 << " Flow Size: " << _flow_size << " Flow " << _name << " time " << timeAsUs(eventlist().now()) << endl;
    p->sendOn();
}

void CCSrc::doNextEvent() {
    if (!_flow_started){
      startflow();
      return;
    }
}

////////////////////////////////////////////////////////////////
//  CC SINK Aici este codul ce ruleaza pe receptor, in mare nu o sa aducem multe modificari
////////////////////////////////////////////////////////////////

/* Only use this constructor when there is only one for to this receiver */
CCSink::CCSink()
    : Logged("CCSINK"), _total_received(0) 
{
    _src = 0;
    
    _nodename = "CCsink";
    _total_received = 0;
}

/* Connect a src to this sink. */ 
void CCSink::connect(CCSrc& src, Route* route)
{
    _src = &src;
    _route = route;
    setName(_src->_nodename);
}


// Receive a packet.
// seqno is the first byte of the new packet.
void CCSink::receivePacket(Packet& pkt) {
    switch (pkt.type()) {
    case CC:
        break;
    default:
        abort();
    }

    CCPacket *p = (CCPacket*)(&pkt);
    CCPacket::seq_t seqno = p->seqno();

    simtime_picosec ts = p->ts();
    //bool last_packet = ((CCPacket*)&pkt)->last_packet();

    if (pkt.header_only()){
        send_nack(ts,seqno);      
    
        p->free();

        //cout << "Wrong seqno received at CC SINK " << seqno << " expecting " << _cumulative_ack << endl;
        return;
    }

    int size = p->size()-ACKSIZE; 
    _total_received += Packet::data_packet_size();;

    send_ack(ts,seqno);
    // have we seen everything yet?
    pkt.free();
}

void CCSink::send_ack(simtime_picosec ts,CCPacket::seq_t ackno) {
    CCAck *ack = 0;
    ack = CCAck::newpkt(_src->_flow, *_route, ackno,ts);
    ack->sendOn();
}

void CCSink::send_nack(simtime_picosec ts, CCPacket::seq_t ackno) {
    CCNack *nack = NULL;
    nack = CCNack::newpkt(_src->_flow, *_route, ackno,ts);
    nack->sendOn();
}
