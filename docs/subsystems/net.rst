networking
==========

``net/`` is the wire. everything in it is about one idea that
took a while to become obvious: **a network protocol is almost entirely
arithmetic over a byte buffer.** a header is a set of offsets, a checksum is
a sum, an address is four bytes. none of it needs a card, and almost none of
it needs a kernel.

so the card is one file (``e1000.c``, in ``drivers/``) and everything else
here is pure. every protocol can be tested against packets written out by
hand, and against real captured ones, which is the only way to be sure the
project's idea of a header matches everybody else's.

byte order
----------

the wire is big-endian and this machine is not. every multi-byte field in
every header has to be turned around, and forgetting one is the single most
common bug in the entire subject: it produces a packet that looks almost
right and is silently discarded by everything that receives it.

so there are **no structs overlaid on packets**. there are explicit
get/put functions at explicit offsets, the same way ``fs/ext4.c`` reads a
superblock and for the same reason: the offsets *are* the format, and a
struct is a claim about padding and order the compiler is free to
reinterpret.

the layers
----------

``net.c``
   addresses, the checksum, byte order. ``net_sum`` accumulates across
   pieces, which is what lets a checksum cover bytes from two different
   places. ``struct ip_header``, ``struct udp_header``, ``struct
   icmp_echo`` are the pulled-apart forms.

``ether.c``
   ethernet, which is fourteen bytes and one idea: a frame says who it is
   for, who it is from, and what is inside, and every layer above answers
   only the last of those. that is why arp and ip can be written without
   either knowing the other exists.

``arp.c``
   the question "who has this address, and where to send to reach them",
   asked by shouting it at everybody. ip addresses are assigned; ethernet
   addresses are burned into cards; nothing connects the two except
   asking. the answer is *remembered* (``ARP_TTL_MS``), because doing it
   for every packet would double the traffic, and that cache is the
   whole of the danger: nothing authenticates a reply, so anybody on the
   wire can claim any address. that is how arp works everywhere, and it is
   worth writing down rather than discovering.

``ip.c``
   ipv4, and only the parts a machine on one wire needs. **no
   fragmentation** (reassembling means holding partial datagrams from
   strangers, with timers, and a fragment is dropped *visibly* rather
   than read as a whole datagram), **no options** (nothing has sent one
   since about 1995, and the header length is checked rather than
   assumed), **no routing** (the roadmap says this machine and whatever
   else is on the wire; anything not local has nowhere to go).

``icmp.c``
   which for this machine is entirely ``ping``. an echo request and an
   echo reply are the same eight-byte header with one number changed and
   the payload copied back. it is the smallest possible round trip, so
   when it works the card, arp, ip and the checksum are all working, and
   when it does not the fault is in one of four things rather than forty.

``udp.c``
   two ports, a length, and a checksum nobody agrees about. worth a file
   of its own for the checksum: it is computed over a *pseudo-header* that
   is not in the datagram and never goes on the wire. source and
   destination addresses, a zero byte, the protocol number, the udp length
   repeated, which binds the datagram to the addresses it was sent
   between, so one delivered to the wrong host fails its checksum instead
   of being accepted.

``tcp.c``
   the first thing here that has to remember what it said. a sequence
   number is **not a packet count**: it counts *bytes*, and the number in a
   segment is the position of its first byte in everything this side has
   ever sent, so a retransmission carries the same number as the original.
   the same stream can arrive as one segment or forty and mean the same
   thing. syn and fin each occupy one number too, which is what makes them
   acknowledgeable by exactly the same arithmetic as data.

   it is 32 bits and it wraps, so nothing may say ``a < b`` about two
   sequence numbers. every comparison goes through ``seq_lt`` and
   friends, which subtract and look at the sign. this is the single
   easiest thing to get wrong in the whole protocol, because it is right
   for the first four billion bytes.

   out-of-order segments are **dropped** rather than held. that costs
   throughput on a lossy wire and nothing on this one, and it removes an
   entire reassembly structure. it is legal: tcp promises to deliver a
   stream in order, not to be clever about how.

``socket.c``
   enough of a socket layer for a program to use: a port number, an owner,
   and a small queue of datagrams that arrived while nobody was asking.
   that queue is the whole of the design, without it a datagram arriving
   a millisecond before the program asks is gone, and with an unbounded
   one anybody on the wire could make the machine allocate until it
   died. so it is a fixed ring, and a datagram arriving at a full one is
   dropped, which is exactly what udp promises anyway. the sender's
   address is kept beside each datagram rather than on the socket, which
   is what makes a udp server a single port rather than a connection per
   talker.

``netif.c``
   the one place that knows there is both a card and a stack, and the only
   file in ``net/`` that is not pure. it decides what to do with the bytes
   the protocols parsed. there is exactly one interface; a machine with
   two would want a routing table, and the roadmap is explicit that there
   is no routing here.

the machine boots with no address and keeps none until it is given one.
until then it says nothing on the wire, which is honest: there is no address
to put in a packet, and every command that needs one says "the wire is not
up". the kernel does not go and ask for one. finding out an address, and
which name server to ask about names, and fetching anything once both are
known, is a client's work, and a client is a program. the monitor's
``ifconfig`` set one by hand until 0.4.4, when the monitor went, so until a
program can hand the kernel what it found out, which the roadmap has next,
the stack has no address at all and answers nothing on the wire.

the card
--------

``drivers/e1000.c`` is the network card. it is the one file here that
touches hardware, and the reason everything else is pure. arp, tcp and the
rest were written before the card existed, which is the point of keeping
the layer above it bytes-in bytes-out.

what is deliberately not here
-----------------------------

no fragmentation, no ip options, no routing, no ipv6, no tls, no tcp
reassembly, and no tcp congestion control worth the name. each of those is
named in ``ROADMAP.md`` as something later or not at all, and each of them
is a version of its own rather than a line in this one.

the netif interface
---------------------

the card, and the state of the wire. frames arrive in a queue and leave
through a send path, and the interface holds what it was told: its address,
its netmask and its router.

``bool net_up(ipv4 address, ipv4 netmask, ipv4 router);``
    what this machine is on the wire, and where whatever is not on it goes.

``ipv4 net_router(void);``
    where anything not on this wire is sent, 0 for nowhere.

``void net_service_ready(void);``
    the poll thread saying which thread it is, so the card has somebody to wake.

``#define NET_FALLBACK_MS 500``
    how long that thread waits when nothing wakes it.

``bool net_send_ip(ipv4 to, uint8_t protocol, const void *payload, size_t len);``
    send a datagram to an address on this wire.

``bool net_arp_request(ipv4 who);``
    ask who has an address, whether or not anything wants to send yet

``size_t net_socket_pending(int owner, int handle);``
    how many datagrams are waiting on a socket. nothing here waits: the
    socket files in ``net/sockets.c`` look again, and a signal ends their
    wait the way it ends any other.

``int64_t net_tcp_recv(int owner, int handle, void *out, size_t max);``
    what has arrived, up to max: how much, 0 at the end of the stream, or
    -1 for nothing yet.

``int net_tcp_error(int owner, int handle);``
    why a connection ended, as an errno, or 0 while it has not.

``bool net_tcp_give(int owner, int handle, int new_owner);``
    a connection handed to another owner, as accept hands one to the new
    socket file it makes.

``int64_t net_tcp_waiting(int owner, int handle);``
    bytes received and not yet read, which is ``FIONREAD``.

``bool net_tcp_ends(int owner, int handle, ipv4 *local, uint16_t *local_port, ipv4 *remote, uint16_t *remote_port);``
    the addresses and ports at either end, for getsockname and getpeername.

``int net_tcp_connect(ipv4 to, uint16_t port);``
    connections live in a fixed table here, driven by the poll thread.


the tcp interface
-------------------

a stream between two machines: they agree on how much each has said,
keep what they have said until the other confirms it, and agree on how to
stop. sequence numbers count bytes rather than packets, which is what lets
the same stream arrive as one segment or forty and mean the same thing.

out-of-order segments are dropped rather than held, which costs nothing on
this wire and removes an entire reassembly structure.

``#define TCP_MSS 1460``
    what fits in one segment.

``#define TCP_SEND_BUF 4096``
    what one connection will hold in each direction.

``#define TCP_CONN_MAX 4``
    how many connections this machine will hold at once.

``bool tcp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to, struct tcp_segment *out);``
    the addresses are needed for the checksum, which covers the same pseudo-header udp uses and for the same reason. false on a bad checksum, a header shorter than 20 bytes, or a data offset that points outside the segment

``size_t tcp_build(uint8_t *p, ipv4 from, ipv4 to, uint16_t from_port, uint16_t to_port, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window, const void *payload, size_t payload_len);``
    write a segment. `payload` may be NULL for a pure ack or a handshake.

``static inline bool seq_lt(uint32_t a, uint32_t b)``
    every comparison of two sequence numbers goes through these.

``enum tcp_state { TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED, TCP_FIN_WAIT_1,     /* the kernel said fin, waiting for it to be acknowledged */ TCP_FIN_WAIT_2,     /* it was; waiting for the other end's fin */``
    eleven states, which sounds like a lot until you see that seven of them are about *stopping*.

``#define TCP_RTO_MS      500``
    how long to wait before repeating something unacknowledged, and how many times.

``#define TCP_TIME_WAIT_MS 10000``
    two minutes is what the rfc asks for.

``uint32_t snd_una, snd_nxt;``
    una: the oldest byte not yet acknowledged.

``uint32_t rcv_nxt;``
    nxt: the next byte expected.

``uint64_t timer_ms;``
    the retransmission timer, which covers whatever is oldest and unacknowledged, one timer for the connection rather than one per segment, which is what every stack does and is enough

``bool ack_due;``
    an acknowledgement is owed.

``bool closing;``
    the local end has finished writing.

``bool done_reading;``
    and has finished *reading*, which is a separate fact and the one that matters more.

``bool reset_due;``
    an abrupt end is owed.

``uint64_t closed_ms;``
    when TIME_WAIT began, or when the connection was reset

``void tcp_listen(struct tcp_conn *c, ipv4 local, uint16_t port, uint32_t iss);``
    wait for somebody to connect.

``void tcp_connect(struct tcp_conn *c, ipv4 local, uint16_t local_port, ipv4 remote, uint16_t remote_port, uint32_t iss, uint64_t now_ms);``
    open one. `iss` is the initial sequence number and should differ between connections: a predictable one lets anybody who can guess it inject data into somebody else's stream, and a *repeated* one lets a straggler from the last connection be accepted by this one

``size_t tcp_write(struct tcp_conn *c, const void *data, size_t len);``
    write and read move bytes in and out of the connection's buffers.

``size_t tcp_writable(const struct tcp_conn *c);``
    how much room is left to write into, and how much is waiting to be read.

``void tcp_close(struct tcp_conn *c);``
    say fin once everything written has gone.

``void tcp_reset(struct tcp_conn *c, uint64_t now_ms);``
    stop now, and tell the other end so.

``size_t tcp_tick(struct tcp_conn *c, uint64_t now_ms, uint8_t *out, size_t max);``
    returns the length of a segment to send, or 0 for nothing to do.

``bool tcp_finished(const struct tcp_conn *c, uint64_t now_ms);``
    whether this connection can be forgotten: closed, and past whatever wait its ending required


the arp interface
-------------------

the question that connects two kinds of address. ethernet addresses are
burned into the cards and ip addresses are assigned, so nothing connects them
except asking: a machine that wants to send to an address broadcasts who has
it, and whoever does answers with their hardware address.

``bool arp_parse(const uint8_t *p, size_t len, struct arp_packet *out);``
    false unless it is ethernet-and-ipv4 arp of the right length.

``size_t arp_build(uint8_t *p, uint16_t op, const struct mac *sender_mac, ipv4 sender_ip, const struct mac *target_mac, ipv4 target_ip);``
    write the 28 bytes. returns how many, so a caller can add it to the ethernet header's length without knowing the number

``#define ARP_TTL_MS (2 * 60 * 1000)``
    how long an answer is believed.

``void arp_learn(struct arp_cache *c, ipv4 ip, const struct mac *mac, uint64_t now_ms);``
    remember one. replaces an existing answer for the same address, since the newest claim is the one everybody acts on

``size_t arp_count(const struct arp_cache *c, uint64_t now_ms);``
    how many answers are still believed


the ether interface
---------------------

the frame itself: two hardware addresses, a type, and a payload. what is
above this is a protocol number and a buffer, and nothing more.

``#define ETHER_HEADER_LEN 14``
    ethernet, which is fourteen bytes and one idea.

``#define ETHERTYPE_IPV4 0x0800``
    the ones this machine speaks. there are hundreds and it needs two

``bool ether_parse(const uint8_t *frame, size_t len, struct ether_header *out);``
    pull the fourteen bytes apart.

``size_t ether_build(uint8_t *frame, const struct mac *to, const struct mac *from, uint16_t type);``
    and put them together.


the icmp interface
--------------------

the protocol that says no: an echo request and its reply, and the error
messages that report a packet that went nowhere.

``#define ICMP_HEADER_LEN 8``
    icmp, which for this machine is entirely `ping`.

``bool icmp_parse_echo(const uint8_t *p, size_t len, struct icmp_echo *out);``
    false unless it is an echo of either kind with a good checksum.

``size_t icmp_build_echo(uint8_t *p, uint8_t type, uint16_t id, uint16_t seq, const void *payload, size_t payload_len);``
    build an echo, request or reply, with `payload` copied in after the header.


the ip interface
------------------

ipv4, and only the parts a machine on one wire needs. no fragmentation,
since reassembling means holding partial datagrams from strangers with
timers, which is a denial of service waiting to happen; no options; and no
routing, because anything not on this wire has nowhere to go.

``#define IP_HEADER_MIN 20``
    ipv4, and only the parts a machine on one wire needs.

``size_t ip_build(uint8_t *p, ipv4 from, ipv4 to, uint8_t protocol, uint16_t payload_len, uint16_t id);``
    write a 20-byte header with the checksum filled in.

``bool ip_same_network(ipv4 a, ipv4 b, ipv4 netmask);``
    are these two on the same wire?

``ipv4 ip_next_hop(ipv4 to, ipv4 me, ipv4 netmask, ipv4 gateway);``
    which machine's hardware address to put on the frame.


the net interface
-------------------

the wire itself: the byte order and checksum helpers every protocol above
needs. a protocol is almost entirely arithmetic over a byte buffer, which is
why there are no structs overlaid on packets here: the offsets are the
format, and a struct is a claim about padding the compiler is free to
reinterpret.

``typedef uint32_t ipv4;``
    an ipv4 address, kept in host order everywhere above the wire.

``bool ipv4_parse(const char *s, ipv4 *out);``
    "10.0.2.15" -> an address.

``uint16_t net_sum_finish(const struct net_sum *s);``
    the finished value, in host order. store it with put16be

``uint16_t net_checksum(const void *data, size_t len);``
    the whole thing in one call, for the common case


the socket interface
----------------------

enough of a socket layer for a program to use: a port number, an owner, and
a small queue of datagrams that arrived while nobody was asking.

the queue is a fixed ring, and a datagram arriving at a full one is dropped,
which is what udp promises anyway: a stack that quietly grew instead would be
making a promise the protocol does not.

``#define SOCKET_MAX      8       /* how many may be open at once */``
    enough of a socket layer for a program to use.

``int socket_open(struct socket_table *t, int owner, uint16_t port);``
    claim a port. -1 if the table is full or somebody already has it, two programs sharing a port would mean a datagram going to whichever was found first, which is worse than refusing

``bool socket_close(struct socket_table *t, int owner, int handle);``
    give one up, and everything a pid holds when it ends

``int64_t socket_take(struct socket_table *t, int owner, int handle, ipv4 *from, uint16_t *from_port, void *out, size_t max);``
    take the oldest waiting datagram.

``bool socket_port(const struct socket_table *t, int owner, int handle, uint16_t *out);``
    which port a handle is bound to, for the sender to put in a datagram


the udp interface
-------------------

the smallest protocol here. eight bytes of header, a length and a checksum,
and no memory of anything.

the checksum is computed over a pseudo-header that is not in the datagram and
never goes on the wire, which is what binds the datagram to the addresses it
was sent between: one delivered to the wrong host by a broken router fails
instead of being accepted by whoever happened to receive it.

``#define UDP_HEADER_LEN 8``
    udp: two ports, a length, and a checksum nobody agrees about.

``bool udp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to, struct udp_header *out);``
    the addresses are needed to check the sum, which is the whole point of the pseudo-header.

``size_t udp_build(uint8_t *p, ipv4 from, ipv4 to, uint16_t from_port, uint16_t to_port, const void *payload, size_t payload_len);``
    write the header and copy the payload after it.
