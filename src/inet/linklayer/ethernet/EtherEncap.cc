/*
 * Copyright (C) 2003 Andras Varga; CTIE, Monash University, Australia
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>

#include "inet/common/INETUtils.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/common/EtherTypeTag_m.h"
#include "inet/linklayer/common/Ieee802Ctrl.h"
#include "inet/linklayer/common/Ieee802SapTag_m.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/linklayer/common/MACAddressTag_m.h"
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ieee8022/Ieee8022LlcHeader_m.h"
#include "inet/networklayer/contract/IInterfaceTable.h"

namespace inet {

Define_Module(EtherEncap);

simsignal_t EtherEncap::encapPkSignal = registerSignal("encapPk");
simsignal_t EtherEncap::decapPkSignal = registerSignal("decapPk");
simsignal_t EtherEncap::pauseSentSignal = registerSignal("pauseSent");

void EtherEncap::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        const char *fcsModeString = par("fcsMode");
        if (!strcmp(fcsModeString, "declaredCorrect"))
            fcsMode = FCS_DECLARED_CORRECT;
        else if (!strcmp(fcsModeString, "declaredIncorrect"))
            fcsMode = FCS_DECLARED_INCORRECT;
        else if (!strcmp(fcsModeString, "computed"))
            fcsMode = FCS_COMPUTED;
        else
            throw cRuntimeError("Unknown crc mode: '%s'", fcsModeString);

        seqNum = 0;
        WATCH(seqNum);
        totalFromHigherLayer = totalFromMAC = totalPauseSent = 0;
        useSNAP = par("useSNAP").boolValue();
        WATCH(totalFromHigherLayer);
        WATCH(totalFromMAC);
        WATCH(totalPauseSent);
    }
}

void EtherEncap::handleMessage(cMessage *msg)
{
    if (msg->arrivedOn("upperLayerIn")) {
        EV_INFO << "Received " << msg << " from upper layer." << endl;
        // from higher layer
        if (msg->isPacket())
            processPacketFromHigherLayer(check_and_cast<Packet *>(msg));
        else
            processCommandFromHigherLayer(msg);
    }
    else if (msg->arrivedOn("lowerLayerIn")) {
        EV_INFO << "Received " << msg << " from lower layer." << endl;
        processFrameFromMAC(check_and_cast<Packet *>(msg));
    }
    else
        throw cRuntimeError("Unknown message");
}

void EtherEncap::processCommandFromHigherLayer(cMessage *msg)
{
    switch (msg->getKind()) {
        case IEEE802CTRL_SENDPAUSE:
            // higher layer want MAC to send PAUSE frame
            handleSendPause(msg);
            break;

        default:
            throw cRuntimeError("Received message `%s' with unknown message kind %d", msg->getName(), msg->getKind());
    }
}

void EtherEncap::refreshDisplay() const
{
    char buf[80];
    sprintf(buf, "passed up: %ld\nsent: %ld", totalFromMAC, totalFromHigherLayer);
    getDisplayString().setTagArg("t", 0, buf);
}

void EtherEncap::processPacketFromHigherLayer(Packet *packet)
{
    if (packet->getByteLength() > MAX_ETHERNET_DATA_BYTES)
        throw cRuntimeError("packet from higher layer (%d bytes) exceeds maximum Ethernet payload length (%d)", (int)packet->getByteLength(), MAX_ETHERNET_DATA_BYTES);

    totalFromHigherLayer++;
    emit(encapPkSignal, packet);

    // Creates MAC header information and encapsulates received higher layer data
    // with this information and transmits resultant frame to lower layer

    // create Ethernet frame, fill it in from Ieee802Ctrl and encapsulate msg in it
    EV_DETAIL << "Encapsulating higher layer packet `" << packet->getName() << "' for MAC\n";

    auto protocolTag = packet->getTag<PacketProtocolTag>();
    const Protocol *protocol = protocolTag ? protocolTag->getProtocol() : nullptr;
    int typeOrLength = 0;
    int ethType = -1;
    int snapType = -1;
    if (protocol) {
        ethType = ProtocolGroup::ethertype.findProtocolNumber(protocol);
        if (ethType == -1) {
            snapType = ProtocolGroup::snapOui.findProtocolNumber(protocol);
        }
    }

    if ((ethType != -1 && useSNAP) || snapType != -1) {
        const auto& snapHeader = makeShared<Ieee8022LlcSnapHeader>();
        if (ethType != -1) {
            snapHeader->setOui(0);
            snapHeader->setProtocolId(ethType);
        }
        else {
            snapHeader->setOui(snapType);
            snapHeader->setProtocolId(-1);      //FIXME get value from a tag (e.g. protocolTag->getSubId() ???)
        }
        packet->insertHeader(snapHeader);

        typeOrLength = packet->getByteLength();
    }
    else if (ethType != -1) {
        typeOrLength = ethType;
    }
    else {
        const auto& llcHeader = makeShared<Ieee8022LlcHeader>();
        int llctype = ProtocolGroup::ieee8022llcprotocol.findProtocolNumber(protocol);
        if (llctype != -1) {
            llcHeader->setSsap((llctype >> 16) & 0xFF);
            llcHeader->setDsap((llctype >> 8) & 0xFF);
            llcHeader->setControl((llctype) & 0xFF);
        }
        else {
            auto sapTag = packet->getMandatoryTag<Ieee802SapReq>();
            llcHeader->setSsap(sapTag->getSsap());
            llcHeader->setDsap(sapTag->getDsap());
            llcHeader->setControl(3);       //TODO get from sapTag
        }
        packet->insertHeader(llcHeader);
        typeOrLength = packet->getByteLength();
    }
    auto macAddressReq = packet->getMandatoryTag<MacAddressReq>();
    const auto& ethHeader = makeShared<EthernetMacHeader>();
    ethHeader->setSrc(macAddressReq->getSrcAddress());    // if blank, will be filled in by MAC
    ethHeader->setDest(macAddressReq->getDestAddress());
    ethHeader->setTypeOrLength(typeOrLength);
    packet->insertHeader(ethHeader);

    EtherEncap::addPaddingAndFcs(packet, fcsMode);

    EV_INFO << "Sending " << packet << " to lower layer.\n";
    send(packet, "lowerLayerOut");
}

void EtherEncap::addPaddingAndFcs(Packet *packet, EthernetFcsMode fcsMode, int64_t requiredMinBytes)
{
    int64_t paddingLength = requiredMinBytes - ETHER_FCS_BYTES - packet->getByteLength();
    if (paddingLength > 0) {
        const auto& ethPadding = makeShared<EthernetPadding>();
        ethPadding->setChunkLength(B(paddingLength));
        ethPadding->markImmutable();
        packet->pushTrailer(ethPadding);
    }
    const auto& ethFcs = makeShared<EthernetFcs>();
    ethFcs->setFcsMode(fcsMode);
    //FIXME calculate Fcs if needed
    ethFcs->markImmutable();
    packet->pushTrailer(ethFcs);
}

const Ptr<const EthernetMacHeader> EtherEncap::decapsulateMacHeader(Packet *packet)
{
    auto ethHeader = packet->popHeader<EthernetMacHeader>();
    packet->popTrailer<EthernetFcs>(B(ETHER_FCS_BYTES));

    // add Ieee802Ctrl to packet
    auto macAddressInd = packet->ensureTag<MacAddressInd>();
    macAddressInd->setSrcAddress(ethHeader->getSrc());
    macAddressInd->setDestAddress(ethHeader->getDest());

    // remove Padding if possible
    if (isIeee802_3Header(*ethHeader)) {
        b payloadLength = B(ethHeader->getTypeOrLength());
        if (packet->getDataLength() < payloadLength)
            throw cRuntimeError("incorrect payload length in ethernet frame");
        packet->setTrailerPopOffset(packet->getHeaderPopOffset() + payloadLength);
        packet->ensureTag<PacketProtocolTag>()->setProtocol(&Protocol::ieee8022llc);
    }
    else if (isEth2Header(*ethHeader)) {
        packet->ensureTag<PacketProtocolTag>()->setProtocol(ProtocolGroup::ethertype.getProtocol(ethHeader->getTypeOrLength()));
    }
    return ethHeader;
}

const Ptr<const EthernetMacHeader> EtherEncap::decapsulateMacLlcSnap(Packet *packet)
{
    const Protocol *payloadProtocol = nullptr;
    auto ethHeader = decapsulateMacHeader(packet);

    // remove llc header if possible
    if (isIeee802_3Header(*ethHeader)) {
        const auto& llcHeader = packet->popHeader<Ieee8022LlcHeader>();

        auto sapInd = packet->ensureTag<Ieee802SapInd>();
        sapInd->setSsap(llcHeader->getSsap());
        sapInd->setDsap(llcHeader->getDsap());
        //TODO control?

        int32_t ssapDsapCtrl = (llcHeader->getSsap() << 16) | (llcHeader->getDsap() << 8) | (llcHeader->getControl() & 0xFF);
        if (ssapDsapCtrl == 0xAAAA03) {
            // snap header
            const auto& snapHeader = dynamicPtrCast<const Ieee8022LlcSnapHeader>(llcHeader);
            if (snapHeader == nullptr)
                throw cRuntimeError("llc/snap error: llc header suggested snap header, but snap header is missing");
            if (snapHeader->getOui() == 0) {
                payloadProtocol = ProtocolGroup::ethertype.getProtocol(snapHeader->getProtocolId());
            }
            else {
                payloadProtocol = ProtocolGroup::snapOui.getProtocol(snapHeader->getOui());
            }
        }
        else {
            payloadProtocol = ProtocolGroup::ieee8022llcprotocol.findProtocol(ssapDsapCtrl);    // do not use getProtocol
        }
    }
    else if (isEth2Header(*ethHeader)) {
        payloadProtocol = ProtocolGroup::ethertype.getProtocol(ethHeader->getTypeOrLength());
    }

    packet->ensureTag<PacketProtocolTag>()->setProtocol(payloadProtocol);
    return ethHeader;
}

void EtherEncap::processFrameFromMAC(Packet *packet)
{
    // decapsulate and attach control info
    const auto& ethHeader = decapsulateMacLlcSnap(packet);

    auto protocol = packet->getMandatoryTag<PacketProtocolTag>()->getProtocol();
    if (protocol)
        packet->ensureTag<DispatchProtocolReq>()->setProtocol(protocol);

    EV_DETAIL << "Decapsulating frame `" << packet->getName() << "', passing up contained packet `"
              << packet->getName() << "' to higher layer\n";

    totalFromMAC++;
    emit(decapPkSignal, packet);

    // pass up to higher layers.
    EV_INFO << "Sending " << packet << " to upper layer.\n";
    send(packet, "upperLayerOut");
}

void EtherEncap::handleSendPause(cMessage *msg)
{
    Ieee802PauseCommand *etherctrl = dynamic_cast<Ieee802PauseCommand *>(msg->getControlInfo());
    if (!etherctrl)
        throw cRuntimeError("PAUSE command `%s' from higher layer received without Ieee802PauseCommand controlinfo", msg->getName());
    MACAddress dest = etherctrl->getDestinationAddress();
    int pauseUnits = etherctrl->getPauseUnits();
    delete msg;

    EV_DETAIL << "Creating and sending PAUSE frame, with duration = " << pauseUnits << " units\n";

    // create Ethernet frame
    char framename[40];
    sprintf(framename, "pause-%d-%d", getId(), seqNum++);
    auto packet = new Packet(framename);
    const auto& frame = makeShared<EthernetPauseFrame>();
    const auto& hdr = makeShared<EthernetMacHeader>();
    frame->setPauseTime(pauseUnits);
    if (dest.isUnspecified())
        dest = MACAddress::MULTICAST_PAUSE_ADDRESS;
    hdr->setDest(dest);
    packet->insertHeader(frame);
    hdr->setTypeOrLength(ETHERTYPE_FLOW_CONTROL);
    packet->insertHeader(hdr);
    EtherEncap::addPaddingAndFcs(packet, fcsMode);

   EV_INFO << "Sending " << frame << " to lower layer.\n";
    send(packet, "lowerLayerOut");

    emit(pauseSentSignal, pauseUnits);
    totalPauseSent++;
}

} // namespace inet

