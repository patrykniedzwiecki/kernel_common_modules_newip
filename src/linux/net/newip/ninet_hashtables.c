// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * NewIP INET
 * An implementation of the TCP/IP protocol suite for the LINUX
 * operating system. NewIP INET is implemented using the BSD Socket
 * interface as the means of communication with the user level.
 *
 * Generic NewIP INET transport hashtables
 *
 * Based on net/ipv4/inet_hashtables.c
 * Based on net/ipv6/inet6_hashtables.c
 * Based on include/net/ip.h
 * Based on include/net/ipv6.h
 * Based on net/core/secure_seq.c
 */
#define pr_fmt(fmt) "NIP: " fmt

#include <linux/module.h>
#include <linux/random.h>

#include <net/nip_addrconf.h>
#include <net/inet_connection_sock.h>
#include <net/inet_hashtables.h>
#include <net/ninet_hashtables.h>
#include <net/secure_seq.h>

static siphash_key_t net_secret __read_mostly;

static __always_inline void net_secret_init(void)
{
	net_get_random_once(&net_secret, sizeof(net_secret));
}

#ifdef CONFIG_INET
static u32 seq_scale(u32 seq)
{
	/* As close as possible to RFC 793, which
	 * suggests using a 250 kHz clock.
	 * Further reading shows this assumes 2 Mb/s networks.
	 * or 10 Mb/s Ethernet, a 1 MHz clock is appropriate.
	 * For 10 Gb/s Ethernet, a 1 GHz clock should be ok, but
	 * we also need to limit the resolution so that the u32 seq
	 * overlaps less than one time per MSL (2 minutes).
	 * Choosing a clock of 64 ns period is OK. (period of 274 s)
	 */
	return seq + (ktime_get_real_ns() >> 6);
}
#endif

__u32 secure_tcp_nip_sequence_number(const __be32 *saddr, const __be32 *daddr,
				     __be16 sport, __be16 dport)
{
	const struct {
		struct nip_addr saddr;
		struct nip_addr daddr;
		__be16 sport;
		__be16 dport;
	} __aligned(SIPHASH_ALIGNMENT) combined = {
		.saddr = *(struct nip_addr *)saddr,
		.daddr = *(struct nip_addr *)daddr,
		.sport = sport,
		.dport = dport,
	};
	u32 hash;

	net_secret_init();
	hash = siphash(&combined, offsetofend(typeof(combined), dport),
		       &net_secret);
	return seq_scale(hash);
}
EXPORT_SYMBOL_GPL(secure_tcp_nip_sequence_number);

u64 secure_newip_port_ephemeral(const __be32 *saddr, const __be32 *daddr,
				__be16 dport)
{
	const struct {
		struct nip_addr saddr;
		struct nip_addr daddr;
		__be16 dport;
	} __aligned(SIPHASH_ALIGNMENT) combined = {
		.saddr = *(struct nip_addr *)saddr,
		.daddr = *(struct nip_addr *)daddr,
		.dport = dport,
	};
	net_secret_init();
	return siphash(&combined, offsetofend(typeof(combined), dport),
		       &net_secret);
}
EXPORT_SYMBOL_GPL(secure_newip_port_ephemeral);

static inline u32 nip_portaddr_hash(const struct net *net,
				    const struct nip_addr *saddr,
				    unsigned int port)
{
	u32 v = (__force u32)saddr->nip_addr_field32[0] ^ (__force u32)saddr->nip_addr_field32[1];

	return jhash_1word(v, net_hash_mix(net)) ^ port;
}

static u32 __nip_addr_jhash(const struct nip_addr *a, const u32 initval)
{
	u32 v = (__force u32)a->nip_addr_field32[0] ^ (__force u32)a->nip_addr_field32[1];

	return jhash_3words(v,
			    (__force u32)a->nip_addr_field32[0],
			    (__force u32)a->nip_addr_field32[1],
			    initval);
}

static struct inet_listen_hashbucket *
ninet_lhash2_bucket_sk(struct inet_hashinfo *h, struct sock *sk)
{
	u32 hash = nip_portaddr_hash(sock_net(sk),
					  &sk->sk_nip_rcv_saddr,
					  inet_sk(sk)->inet_num);
	return inet_lhash2_bucket(h, hash);
}

static void ninet_hash2(struct inet_hashinfo *h, struct sock *sk)
{
	struct inet_listen_hashbucket *ilb2;

	if (!h->lhash2)
		return;

	ilb2 = ninet_lhash2_bucket_sk(h, sk);

	spin_lock(&ilb2->lock);
	hlist_add_head_rcu(&inet_csk(sk)->icsk_listen_portaddr_node, &ilb2->head);

	ilb2->count++;
	spin_unlock(&ilb2->lock);
}

/* Function
 *    Returns the hash value based on the passed argument
 * Parameter
 *    net: The namespace
 *    laddr: The destination address
 *    lport: Destination port
 *    faddr: Source address
 *    fport: Source port
 */
u32 ninet_ehashfn(const struct net *net,
		  const struct nip_addr *laddr, const u16 lport,
		  const struct nip_addr *faddr, const __be16 fport)
{
	static u32 ninet_ehash_secret __read_mostly;
	static u32 ninet_hash_secret __read_mostly;

	u32 lhash, fhash;

	net_get_random_once(&ninet_ehash_secret, sizeof(ninet_ehash_secret));
	net_get_random_once(&ninet_hash_secret, sizeof(ninet_hash_secret));

	/* Ipv6 uses S6_ADdr32 [3], the last 32bits of the address */
	lhash = (__force u32)laddr->nip_addr_field32[0];
	fhash = __nip_addr_jhash(faddr, ninet_hash_secret);

	return __ninet_ehashfn(lhash, lport, fhash, fport,
			       ninet_ehash_secret + net_hash_mix(net));
}

/* Function
 *    The socket is put into the Listen hash in case the server finds
 *    the socket in the second handshake
 * Parameter
 *    sk: Transmission control block
 *    osk: old socket
 */
int __ninet_hash(struct sock *sk, struct sock *osk)
{
	struct inet_hashinfo *hashinfo = sk->sk_prot->h.hashinfo;
	struct inet_listen_hashbucket *ilb;
	int err = 0;

	if (sk->sk_state != TCP_LISTEN) {
		inet_ehash_nolisten(sk, osk, NULL);
		return 0;
	}
	WARN_ON(!sk_unhashed(sk));
	ilb = &hashinfo->listening_hash[inet_sk_listen_hashfn(sk)];

	spin_lock(&ilb->lock);

	__sk_nulls_add_node_rcu(sk, &ilb->nulls_head);

	ninet_hash2(hashinfo, sk);
	ilb->count++;
	sock_set_flag(sk, SOCK_RCU_FREE);
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);

	spin_unlock(&ilb->lock);

	return err;
}

int ninet_hash(struct sock *sk)
{
	int err = 0;

	if (sk->sk_state != TCP_CLOSE) {
		local_bh_disable();
		err = __ninet_hash(sk, NULL);
		local_bh_enable();
	}

	return err;
}

static void ninet_unhash2(struct inet_hashinfo *h, struct sock *sk)
{
	struct inet_listen_hashbucket *ilb2;

	if (!h->lhash2 ||
	    WARN_ON_ONCE(hlist_unhashed(&inet_csk(sk)->icsk_listen_portaddr_node)))
		return;

	ilb2 = ninet_lhash2_bucket_sk(h, sk);

	spin_lock(&ilb2->lock);
	hlist_del_init_rcu(&inet_csk(sk)->icsk_listen_portaddr_node);
	ilb2->count--;
	spin_unlock(&ilb2->lock);
}

void ninet_unhash(struct sock *sk)
{
	struct inet_hashinfo *hashinfo = sk->sk_prot->h.hashinfo;
	struct inet_listen_hashbucket *ilb = NULL;
	spinlock_t *lock; /* Spin lock (note deleted alarm) */

	if (sk_unhashed(sk))
		return;

	if (sk->sk_state == TCP_LISTEN) {
		ilb = &hashinfo->listening_hash[inet_sk_listen_hashfn(sk)];
		lock = &ilb->lock;
	} else {
		lock = inet_ehash_lockp(hashinfo, sk->sk_hash);
	}
	spin_lock_bh(lock);
	if (sk_unhashed(sk))
		goto unlock;

	if (ilb) {
		ninet_unhash2(hashinfo, sk);
		ilb->count--;
	}
	__sk_nulls_del_node_init_rcu(sk);
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, -1);

unlock:
	spin_unlock_bh(lock);
}

/* Function
 *    Find transport control blocks based on address and port in the ehash table.
 *    If found, three handshakes have been made and a connection has been established,
 *    and normal communication can proceed.
 * Parameter
 *    net: The namespace
 *    hashinfo: A global scalar of type tcp_hashinfo that stores tcp_SOCK(including ESTABLISHED,
 *              listen, and bind) for various states of the current system.
 *    saddr: Source address
 *    sport: Source port
 *    daddr: The destination address
 *    hnum: Destination port
 */
struct sock *__ninet_lookup_established(struct net *net,
					struct inet_hashinfo *hashinfo,
					   const struct nip_addr *saddr,
					   const __be16 sport,
					   const struct nip_addr *daddr,
					   const u16 hnum,
					   const int dif)
{
	struct sock *sk;
	const struct hlist_nulls_node *node;

	const __portpair ports = INET_COMBINED_PORTS(sport, hnum);

	unsigned int hash = ninet_ehashfn(net, daddr, hnum, saddr, sport);
	unsigned int slot = hash & hashinfo->ehash_mask;

	struct inet_ehash_bucket *head = &hashinfo->ehash[slot];

begin:
	sk_nulls_for_each_rcu(sk, node, &head->chain) {
		if (sk->sk_hash != hash)
			continue;
		if (!NINET_MATCH(sk, net, saddr, daddr, ports, dif))
			continue;
		if (unlikely(!refcount_inc_not_zero(&sk->sk_refcnt))) {
			nip_dbg("[nip]%s:sk->sk_refcnt == 0", __func__);
			goto out;
		}

		if (unlikely(!NINET_MATCH(sk, net, saddr, daddr, ports, dif))) {
			sock_gen_put(sk);
			goto begin;
		}
		goto found;
	}
	if (get_nulls_value(node) != slot)
		goto begin;
out:
	sk = NULL;
found:
	return sk;
}

static inline int nip_tcp_compute_score(struct sock *sk, struct net *net,
					const unsigned short hnum,
					const struct nip_addr *daddr,
					const int dif, int sdif)
{
	int score = -1;

	if (inet_sk(sk)->inet_num == hnum && sk->sk_family == PF_NINET &&
	    net_eq(sock_net(sk), net)) {
		score = 1;
		if (!nip_addr_eq(&sk->sk_nip_rcv_saddr, &nip_any_addr)) {
			if (!nip_addr_eq(&sk->sk_nip_rcv_saddr, daddr))
				return -1;
			score++;
		}
		if (!inet_sk_bound_dev_eq(net, sk->sk_bound_dev_if, dif, sdif))
			return -1;
		score++;
		if (READ_ONCE(sk->sk_incoming_cpu) == raw_smp_processor_id())
			score++;
	}

	return score;
}

/* nip reuseport */
static struct sock *ninet_lhash2_lookup(struct net *net,
					struct inet_listen_hashbucket *ilb2,
					struct sk_buff *skb, int doff,
					const struct nip_addr *saddr, __be16 sport,
					const struct nip_addr *daddr, const unsigned short hnum,
					const int dif, const int sdif)
{
	struct inet_connection_sock *icsk;
	struct sock *sk;
	struct sock *result = NULL;
	int hiscore = 0;
	int matches = 0;
	int reuseport = 0;
	u32 phash = 0;

	inet_lhash2_for_each_icsk_rcu(icsk, &ilb2->head) {
		int score;

		sk = (struct sock *)icsk;
		score = nip_tcp_compute_score(sk, net, hnum, daddr, dif, sdif);
		if (score > hiscore) {
			nip_dbg("%s: find sock in lhash table", __func__);
			result = sk;
			hiscore = score;
			reuseport = sk->sk_reuseport;
			if (reuseport) {
				nip_dbg("%s: find reuseport sock in lhash table", __func__);
				phash = ninet_ehashfn(net, daddr, hnum, saddr, sport);
				matches = 1;
			}
		} else if (score == hiscore && reuseport) {
			matches++;
			if (reciprocal_scale(phash, matches) == 0)
				result = sk;
			phash = next_pseudo_random32(phash);
		}
	}
	return result;
}

struct sock *ninet_lookup_listener(struct net *net,
				   struct inet_hashinfo *hashinfo,
				   struct sk_buff *skb, int doff,
				   const struct nip_addr *saddr,
				   const __be16 sport, const struct nip_addr *daddr,
				   const unsigned short hnum, const int dif, const int sdif)
{
	struct inet_listen_hashbucket *ilb2;
	struct sock *result = NULL;
	unsigned int hash2 = nip_portaddr_hash(net, daddr, hnum);

	ilb2 = inet_lhash2_bucket(hashinfo, hash2);

	result = ninet_lhash2_lookup(net, ilb2, skb, doff,
				     saddr, sport, daddr, hnum,
				     dif, sdif);
	if (result)
		goto done;

	hash2 = nip_portaddr_hash(net, &nip_any_addr, hnum);
	ilb2 = inet_lhash2_bucket(hashinfo, hash2);

	result = ninet_lhash2_lookup(net, ilb2, skb, doff,
				     saddr, sport, &nip_any_addr, hnum,
				     dif, sdif);
done:
	if (IS_ERR(result))
		return NULL;
	return result;
}

/* Check whether the quad information in sock is bound by ehash. If not,
 * the SK is inserted into the ehash and 0 is returned
 */
static int __ninet_check_established(struct inet_timewait_death_row *death_row,
				     struct sock *sk, const __u16 lport,
				     struct inet_timewait_sock **twp)
{
	struct inet_hashinfo *hinfo = death_row->hashinfo;
	struct inet_sock *inet = inet_sk(sk);
	struct nip_addr *daddr = &sk->sk_nip_rcv_saddr;
	struct nip_addr *saddr = &sk->sk_nip_daddr;
	int dif = sk->sk_bound_dev_if;
	struct net *net = sock_net(sk);
	const __portpair ports = INET_COMBINED_PORTS(inet->inet_dport, lport);
	unsigned int hash = ninet_ehashfn(net, daddr, lport,
					 saddr, inet->inet_dport);
	struct inet_ehash_bucket *head = inet_ehash_bucket(hinfo, hash);
	spinlock_t *lock = inet_ehash_lockp(hinfo, hash);
	struct sock *sk2;
	const struct hlist_nulls_node *node;

	spin_lock(lock);

	sk_nulls_for_each(sk2, node, &head->chain) {
		if (sk2->sk_hash != hash)
			continue;

		if (likely(NINET_MATCH(sk2, net,
				       saddr, daddr, ports, dif))) {
			nip_dbg("%s: found same sk in ehash", __func__);
			goto not_unique;
		}
	}

	/* Must record num and sport now. Otherwise we will see
	 * in hash table socket with a funny identity.
	 */
	nip_dbg("%s: add tcp sock into ehash table. sport=%u", __func__, lport);
	inet->inet_num = lport;
	inet->inet_sport = htons(lport);
	sk->sk_hash = hash;
	WARN_ON(!sk_unhashed(sk));
	__sk_nulls_add_node_rcu(sk, &head->chain);

	spin_unlock(lock);
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);
	return 0;

not_unique:
	spin_unlock(lock);
	return -EADDRNOTAVAIL;
}

static u64 ninet_sk_port_offset(const struct sock *sk)
{
	const struct inet_sock *inet = inet_sk(sk);

	return secure_newip_port_ephemeral(sk->sk_nip_rcv_saddr.nip_addr_field32,
					  sk->sk_nip_daddr.nip_addr_field32,
					  inet->inet_dport);
}

/* Bind local ports randomly */
int ninet_hash_connect(struct inet_timewait_death_row *death_row,
		       struct sock *sk)
{
	u64 port_offset = 0;

	if (!inet_sk(sk)->inet_num)
		port_offset = ninet_sk_port_offset(sk);

	return __inet_hash_connect(death_row, sk, port_offset,
	__ninet_check_established);
}

