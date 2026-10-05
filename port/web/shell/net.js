// The network between the players' pages: each page is a machine on a LAN
// (10.x.y.z), and the game's system link runs on it as on Xbox consoles.
//
// The pages find each other through the site's MQTT broker (a room of the
// site's players), then talk over WebRTC data channels: one unreliable and
// unordered for datagrams (UDP), one reliable for connections (TCP). Until a
// peer's channels open, the broker relays.
//
// The game writes what it sends into a ring in its memory, and reads what
// arrives from another (port/web/src/posix_bridge.c), in records of the bridge's format.
'use strict';

const HaloNet = (() => {
  const RECORD_DATAGRAM = 1;
  const PEER_TIMEOUT_MILLISECONDS = 10000;
  const HELLO_INTERVAL_MILLISECONDS = 2000;
  const ICE_SERVERS = [{ urls: 'stun:stun.l.google.com:19302' }];

  // ---------- MQTT 3.1.1 over a WebSocket: what signalling needs

  class Mqtt {
    constructor(url, clientId, onMessage, onConnect) {
      this.url = url;
      this.clientId = clientId;
      this.onMessage = onMessage;
      this.onConnect = onConnect;
      this.subscriptions = [];
      this.packetId = 1;
      this.connected = false;
      this.open();
    }

    open() {
      this.socket = new WebSocket(this.url, 'mqtt');
      this.socket.binaryType = 'arraybuffer';
      this.buffer = new Uint8Array(0);
      this.socket.onopen = () => {
        const id = new TextEncoder().encode(this.clientId);
        const body = [0, 4, 77, 81, 84, 84, 4, 2, 0, 60, id.length >> 8, id.length & 255, ...id];
        this.send(0x10, body);
      };
      this.socket.onmessage = (event) => this.receive(new Uint8Array(event.data));
      this.socket.onclose = () => {
        this.connected = false;
        clearInterval(this.ping);
        setTimeout(() => this.open(), 2000);
      };
      this.socket.onerror = () => {};
    }

    static encodeLength(length) {
      const bytes = [];
      do {
        let byte = length % 128;
        length = Math.floor(length / 128);
        if (length > 0) byte |= 128;
        bytes.push(byte);
      } while (length > 0);
      return bytes;
    }

    send(header, body) {
      if (this.socket.readyState !== WebSocket.OPEN) return;
      const length = Mqtt.encodeLength(body.length);
      const packet = new Uint8Array(1 + length.length + body.length);
      packet[0] = header;
      packet.set(length, 1);
      packet.set(body, 1 + length.length);
      this.socket.send(packet);
    }

    receive(data) {
      const joined = new Uint8Array(this.buffer.length + data.length);
      joined.set(this.buffer);
      joined.set(data, this.buffer.length);
      this.buffer = joined;
      for (;;) {
        if (this.buffer.length < 2) return;
        let length = 0, multiplier = 1, index = 1, byte;
        do {
          if (index >= this.buffer.length) return;
          byte = this.buffer[index++];
          length += (byte & 127) * multiplier;
          multiplier *= 128;
        } while (byte & 128);
        if (this.buffer.length < index + length) return;
        const type = this.buffer[0] >> 4;
        const body = this.buffer.subarray(index, index + length);
        this.handle(type, this.buffer[0], body);
        this.buffer = this.buffer.slice(index + length);
      }
    }

    handle(type, header, body) {
      if (type === 2) {          // CONNACK
        this.connected = true;
        for (const topic of this.subscriptions) this.sendSubscribe(topic);
        this.ping = setInterval(() => this.send(0xC0, []), 30000);
        if (this.onConnect) this.onConnect();
      } else if (type === 3) {   // PUBLISH (QoS 0)
        const topicLength = (body[0] << 8) | body[1];
        const topic = new TextDecoder().decode(body.subarray(2, 2 + topicLength));
        this.onMessage(topic, body.slice(2 + topicLength));
      }
    }

    sendSubscribe(topic) {
      const bytes = new TextEncoder().encode(topic);
      const id = this.packetId++ & 0xFFFF || 1;
      this.send(0x82, [id >> 8, id & 255, bytes.length >> 8, bytes.length & 255, ...bytes, 0]);
    }

    subscribe(topic) {
      this.subscriptions.push(topic);
      if (this.connected) this.sendSubscribe(topic);
    }

    publish(topic, payload) {
      const bytes = new TextEncoder().encode(topic);
      const data = typeof payload === 'string' ? new TextEncoder().encode(payload) : payload;
      const body = new Uint8Array(2 + bytes.length + data.length);
      body[0] = bytes.length >> 8;
      body[1] = bytes.length & 255;
      body.set(bytes, 2);
      body.set(data, 2 + bytes.length);
      this.send(0x30, body);
    }
  }

  // ---------- state

  let memory = null;
  let layout = null;
  let base = 0;
  let mqtt = null;
  let topicBase = '';
  const myId = Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => b.toString(16).padStart(2, '0')).join('');
  let myAddress = 0;
  const peers = new Map();          // id -> peer
  let chatListener = null;

  function ring(which) {
    const address = base + layout[which];
    // Int32Array: Atomics.wait and notify take only signed views
    return {
      write: new Int32Array(memory, address + layout[3], 1),
      read: new Int32Array(memory, address + layout[4], 1),
      records: address + layout[5],
    };
  }

  function addressText(address) {
    return [address & 255, (address >>> 8) & 255, (address >>> 16) & 255, address >>> 24].join('.');
  }

  // ---------- records from the game

  function routeRecord(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const type = bytes[2];
    const to = view.getUint32(4, true);
    // the receiver sees the sender's address
    view.setUint32(4, myAddress, true);
    const broadcast = to === 0xFFFFFFFF || (to & 0xFF) === 10 && (to >>> 8) === 0xFFFFFF;
    for (const peer of peers.values()) {
      if (broadcast ? type === RECORD_DATAGRAM : peer.address === to) {
        sendToPeer(peer, bytes, type === RECORD_DATAGRAM);
        if (!broadcast) break;
      }
    }
  }

  function pumpOutgoing() {
    const outgoing = ring(1);
    const size = layout[7];
    let read = Atomics.load(outgoing.read, 0);
    const write = Atomics.load(outgoing.write, 0);
    while (read !== write) {
      const slot = outgoing.records + ((read >>> 0) % layout[6]) * size;
      const header = new Uint8Array(memory, slot, layout[8]);
      const length = header[0] | (header[1] << 8);
      // a copy: the ring's memory is shared, and channels take their own buffers
      routeRecord(new Uint8Array(memory, slot, layout[8] + length).slice());
      read = (read + 1) | 0;
    }
    Atomics.store(outgoing.read, 0, read);
  }

  function waitForOutgoing() {
    const outgoing = ring(1);
    if (Atomics.waitAsync) {
      const value = Atomics.load(outgoing.write, 0);
      if (value === Atomics.load(outgoing.read, 0)) {
        const result = Atomics.waitAsync(outgoing.write, 0, value, 250);
        const next = () => { pumpOutgoing(); waitForOutgoing(); };
        if (result.async) result.value.then(next); else next();
        return;
      }
      pumpOutgoing();
      queueMicrotask(waitForOutgoing);
      return;
    }
    pumpOutgoing();
    setTimeout(waitForOutgoing, 2);
  }

  // ---------- records to the game

  // A datagram that finds the game's ring full is dropped, as a network
  // would; a connection's records wait, in order, until there is room
  // (a dropped piece of a stream corrupted it).
  const backlog = [];
  let backlogTimer = 0;

  function flushBacklog() {
    backlogTimer = 0;
    while (backlog.length && put(backlog[0])) backlog.shift();
    if (backlog.length) backlogTimer = setTimeout(flushBacklog, 4);
  }

  function deliver(bytes) {
    const datagram = bytes[2] === 1;
    if (!datagram && backlog.length) {
      backlog.push(bytes);
      return;
    }
    if (!put(bytes) && !datagram) {
      backlog.push(bytes);
      if (!backlogTimer) backlogTimer = setTimeout(flushBacklog, 4);
    }
  }

  function put(bytes) {
    const incoming = ring(2);
    const write = Atomics.load(incoming.write, 0);
    if (((write - Atomics.load(incoming.read, 0)) >>> 0) >= layout[6]) return false;
    const slot = incoming.records + ((write >>> 0) % layout[6]) * layout[7];
    new Uint8Array(memory, slot, bytes.length).set(bytes.subarray(0, layout[7]));
    Atomics.store(incoming.write, 0, (write + 1) | 0);
    Atomics.notify(incoming.write, 0);
    return true;
  }

  // ---------- peers

  function sendToPeer(peer, bytes, datagram) {
    const channel = datagram ? peer.datagrams : peer.streams;
    if (channel && channel.readyState === 'open' && (!datagram || channel.bufferedAmount < 256 * 1024)) {
      channel.send(bytes);
    } else {
      // through the broker, until the channels open
      mqtt.publish(`${topicBase}/relay/${peer.id}`, bytes);
    }
  }

  function signal(peer, message) {
    message.from = myId;
    mqtt.publish(`${topicBase}/signal/${peer.id}`, JSON.stringify(message));
  }

  function setUpChannel(peer, channel) {
    channel.binaryType = 'arraybuffer';
    channel.onmessage = (event) => deliver(new Uint8Array(event.data));
    if (channel.label === 'datagrams') peer.datagrams = channel;
    else peer.streams = channel;
  }

  function connectPeer(peer) {
    if (typeof RTCPeerConnection === 'undefined') return;
    const connection = new RTCPeerConnection({ iceServers: ICE_SERVERS });
    peer.connection = connection;
    connection.onicecandidate = (event) => {
      if (event.candidate) signal(peer, { candidate: event.candidate.toJSON() });
    };
    connection.ondatachannel = (event) => setUpChannel(peer, event.channel);
    connection.onconnectionstatechange = () => {
      if (connection.connectionState === 'failed') {
        // the broker carries this peer's traffic
        connection.close();
      }
    };
    // the page with the smaller identifier offers
    if (myId < peer.id) {
      setUpChannel(peer, connection.createDataChannel('datagrams', { ordered: false, maxRetransmits: 0 }));
      setUpChannel(peer, connection.createDataChannel('streams', { ordered: true }));
      connection.createOffer()
        .then((offer) => connection.setLocalDescription(offer))
        .then(() => signal(peer, { description: connection.localDescription.toJSON() }))
        .catch((error) => console.log('offer failed: ' + error));
    }
  }

  async function onSignal(message) {
    const peer = peers.get(message.from);
    if (!peer || !peer.connection) return;
    const connection = peer.connection;
    try {
      if (message.description) {
        await connection.setRemoteDescription(message.description);
        if (message.description.type === 'offer') {
          await connection.setLocalDescription(await connection.createAnswer());
          signal(peer, { description: connection.localDescription.toJSON() });
        }
        for (const candidate of peer.pendingCandidates || []) await connection.addIceCandidate(candidate);
        peer.pendingCandidates = null;
      } else if (message.candidate) {
        if (connection.remoteDescription) await connection.addIceCandidate(message.candidate);
        else (peer.pendingCandidates = peer.pendingCandidates || []).push(message.candidate);
      }
    } catch (error) {
      console.log('signalling failed: ' + error);
    }
  }

  function onHello(message) {
    if (message.id === myId) return;
    let peer = peers.get(message.id);
    if (!peer) {
      peer = { id: message.id, address: message.address, name: message.name };
      peers.set(peer.id, peer);
      console.log(`network: ${addressText(peer.address)} joined`);
      connectPeer(peer);
      // answer at once, so the new page knows of this one
      hello();
    }
    peer.lastSeen = performance.now();
    peer.address = message.address;
  }

  function dropPeer(peer) {
    if (peer.connection) peer.connection.close();
    peers.delete(peer.id);
    console.log(`network: ${addressText(peer.address)} left`);
  }

  function hello() {
    mqtt.publish(`${topicBase}/hello`, JSON.stringify({ id: myId, address: myAddress }));
  }

  function onMessage(topic, payload) {
    const name = topic.slice(topicBase.length + 1);
    if (name === 'hello') {
      onHello(JSON.parse(new TextDecoder().decode(payload)));
    } else if (name === `signal/${myId}`) {
      onSignal(JSON.parse(new TextDecoder().decode(payload)));
    } else if (name === `relay/${myId}`) {
      deliver(payload);
    } else if (name === 'chat' && chatListener) {
      const message = JSON.parse(new TextDecoder().decode(payload));
      if (message.id !== myId) chatListener(message.name, message.text);
    } else if (name === 'bye') {
      const message = JSON.parse(new TextDecoder().decode(payload));
      const peer = peers.get(message.id);
      if (peer) dropPeer(peer);
    }
  }

  // ---------- public

  function attach(sharedMemory, address, netLayout, config) {
    memory = sharedMemory;
    base = address;
    layout = netLayout;
    myAddress = new Uint32Array(memory, base + layout[0], 1)[0];
    const room = (config && config.room) || 'halo';
    topicBase = `halo-web/v2/${room}`;
    let broker = (config && config.broker) || '/mqtt';
    if (broker.startsWith('/')) broker = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + broker;
    mqtt = new Mqtt(broker, 'halo-' + myId, onMessage, hello);
    for (const name of ['hello', `signal/${myId}`, `relay/${myId}`, 'chat', 'bye']) mqtt.subscribe(`${topicBase}/${name}`);
    setInterval(() => {
      if (mqtt.connected) hello();
      const now = performance.now();
      for (const peer of peers.values()) {
        if (now - peer.lastSeen > PEER_TIMEOUT_MILLISECONDS) dropPeer(peer);
      }
    }, HELLO_INTERVAL_MILLISECONDS);
    addEventListener('pagehide', () => {
      if (mqtt.connected) mqtt.publish(`${topicBase}/bye`, JSON.stringify({ id: myId }));
    });
    console.log(`network: this page is ${addressText(myAddress)} in room ${room}`);
    waitForOutgoing();
  }

  function sendChat(name, text) {
    if (mqtt && mqtt.connected) mqtt.publish(`${topicBase}/chat`, JSON.stringify({ id: myId, name, text }));
  }

  function onChat(listener) {
    chatListener = listener;
  }

  function peerCount() {
    return peers.size;
  }

  return { attach, sendChat, onChat, peerCount };
})();
