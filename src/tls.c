#include "types.h"
#include "tls.h"
#include "config.h"
#include "logging.h"
#include "utils.h"

#ifndef KERNEL_SPACE
#include <fcntl.h>
#include <unistd.h>
#endif

static int bruteforce_analyze_sni_str(
	const struct section_config_t *section,
	const uint8_t *data, size_t dlen,
	struct tls_verdict *vrd
) {
	if (section->all_domains) {
		vrd->target_sni = 1;
		vrd->sni_len = 0;
		vrd->sni_offset = dlen / 2;
		return 0;
	}

	for (struct domains_list *sne = section->sni_domains; sne != NULL; sne = sne->next) {
		const char *domain_startp = sne->domain_name;
		int domain_len = sne->domain_len;

		if (sne->domain_len + dlen + 1 > MAX_PACKET_SIZE) { 
			continue;
		}

		NETBUF_ALLOC(buf, MAX_PACKET_SIZE);
		if (!NETBUF_CHECK(buf)) {
			lgerror(-ENOMEM, "Allocation error");
			return -ENOMEM;
		}
		NETBUF_ALLOC(nzbuf, MAX_PACKET_SIZE * sizeof(int));
		if (!NETBUF_CHECK(nzbuf)) {
			lgerror(-ENOMEM, "Allocation error");
			NETBUF_FREE(buf);
			return -ENOMEM;
		}

		int *zbuf = (void *)nzbuf;

		memcpy(buf, domain_startp, domain_len);
		memcpy(buf + domain_len, "#", 1);
		memcpy(buf + domain_len + 1, data, dlen);

		z_function((char *)buf, zbuf, domain_len + 1 + dlen);

		for (unsigned int k = 0; k < dlen; k++) {
			if (zbuf[k] == domain_len) {
				vrd->target_sni = 1;
				vrd->sni_len = domain_len;
				vrd->sni_offset = (k - domain_len - 1);
				vrd->sni_target_offset = vrd->sni_offset;
				vrd->sni_target_len = vrd->sni_len;
				NETBUF_FREE(buf);
				NETBUF_FREE(nzbuf);
				return 0;
			}
		}


		NETBUF_FREE(buf);
		NETBUF_FREE(nzbuf);
	}

	return 0;
}
static int analyze_sni_str(
	const struct section_config_t *section,
	const char *sni_name, int sni_len, const uint8_t *data, 
	struct tls_verdict *vrd
) {
	if (section->all_domains) {
		vrd->target_sni = 1;
		goto check_domain;
	}
		
	for (struct domains_list *sne = section->sni_domains; sne != NULL; sne = sne->next) {
		const char *sni_startp = sni_name + sni_len - sne->domain_len;
		const char *domain_startp = sne->domain_name;

		if (sni_len >= sne->domain_len &&
			sni_len < 128 && 
			!strncmp(sni_startp, 
			domain_startp, 
			sne->domain_len)) {
				vrd->target_sni = 1;
				vrd->sni_target_offset = (const uint8_t *)sni_startp - data;
				vrd->sni_target_len = sne->domain_len;
				break;
		}
	}

check_domain:
	if (vrd->target_sni == 1) {
		for (struct domains_list *sne = section->exclude_sni_domains; sne != NULL; sne = sne->next) {
			const char *sni_startp = sni_name + sni_len - sne->domain_len;
			const char *domain_startp = sne->domain_name;

			if (sni_len >= sne->domain_len &&
				sni_len < 128 && 
				!strncmp(sni_startp, 
				domain_startp, 
				sne->domain_len)) {
					vrd->target_sni = 0;
					lgdebugmsg("Excluded SNI: %.*s", 
						vrd->sni_len, data + vrd->sni_offset);
			}
		}
	}

	return 0;
}


#define TLS_CONTENT_TYPE_HANDSHAKE 0x16
#define TLS_HANDSHAKE_TYPE_CLIENT_HELLO 0x01
#define TLS_EXTENSION_SNI 0x0000
#define TLS_EXTENSION_CLIENT_HELLO_ENCRYPTED 0xfe0d

/**
 * Processes tls payload of the tcp request.
 * 
 * data Payload data of TCP.
 * dlen Length of `data`.
 */
struct tls_verdict analyze_tls_data(
	const struct section_config_t *section,
	const uint8_t *data, 
	uint32_t dlen) 
{
	struct tls_verdict vrd = {0};

	size_t i = 0;
	const uint8_t *data_end = data + dlen;

	if (section->sni_detection == SNI_DETECTION_BRUTE) {
		bruteforce_analyze_sni_str(section, data, dlen, &vrd);
		goto out;
	}

	while (i + 4 < dlen) {
		const uint8_t *msgData = data + i;

		uint8_t tls_content_type = *msgData;
		uint8_t tls_vmajor = *(msgData + 1);
		uint16_t message_length = ntohs(*(uint16_t *)(msgData + 3));

		if (tls_vmajor != 0x03) goto nextMessage;

		if (i + 5 > dlen) break;

		if (tls_content_type != TLS_CONTENT_TYPE_HANDSHAKE) 
			goto nextMessage;

		const uint8_t *handshakeProto = msgData + 5;

		if (handshakeProto + 1 >= data_end) break;

		uint8_t handshakeType = *handshakeProto;

		if (handshakeType != TLS_HANDSHAKE_TYPE_CLIENT_HELLO)
			goto nextMessage;

		const uint8_t *msgPtr = handshakeProto;
		msgPtr += 1; 
		msgPtr += 3 + 2 + 32;

		if (msgPtr + 1 >= data_end) break;
		uint8_t sessionIdLength = *msgPtr;
		msgPtr++;
		msgPtr += sessionIdLength;

		if (msgPtr + 2 >= data_end) break;
		uint16_t ciphersLength = ntohs(*(uint16_t *)msgPtr);
		msgPtr += 2;
		msgPtr += ciphersLength;

		if (msgPtr + 1 >= data_end) break;
		uint8_t compMethodsLen = *msgPtr;
		msgPtr++;
		msgPtr += compMethodsLen;

		if (msgPtr + 2 >= data_end) break;
		uint16_t extensionsLen = ntohs(*(uint16_t *)msgPtr);
		msgPtr += 2;

		const uint8_t *extensionsPtr = msgPtr;
		const uint8_t *extensions_end = extensionsPtr + extensionsLen;
		if (extensions_end > data_end) extensions_end = data_end;

		while (extensionsPtr < extensions_end) {
			const uint8_t *extensionPtr = extensionsPtr;
			if (extensionPtr + 4 >= extensions_end) break;

			uint16_t extensionType = 
				ntohs(*(uint16_t *)extensionPtr);
			extensionPtr += 2;

			uint16_t extensionLen = 
				ntohs(*(uint16_t *)extensionPtr);
			extensionPtr += 2;


			if (extensionPtr + extensionLen > extensions_end) 
				break;

			if (extensionType != TLS_EXTENSION_SNI) 
				goto nextExtension;

			const uint8_t *sni_ext_ptr = extensionPtr;

			if (sni_ext_ptr + 2 >= extensions_end) break;
			uint16_t sni_ext_dlen = ntohs(*(uint16_t *)sni_ext_ptr);

			sni_ext_ptr += 2;

			const uint8_t *sni_ext_end = sni_ext_ptr + sni_ext_dlen;
			if (sni_ext_end >= extensions_end) break;
			
			if (sni_ext_ptr + 3 >= sni_ext_end) break;
			sni_ext_ptr++;
			uint16_t sni_len = ntohs(*(uint16_t *)sni_ext_ptr);
			sni_ext_ptr += 2;

			if (sni_ext_ptr + sni_len > sni_ext_end) break;

			const char *sni_name = (char *)sni_ext_ptr;

			vrd.sni_offset = (uint8_t *)sni_name - data;
			vrd.sni_target_offset = vrd.sni_offset;
			vrd.sni_len = sni_len;
			vrd.sni_target_len = vrd.sni_len;

			analyze_sni_str(section, sni_name, sni_len, data, &vrd);
			goto out;

nextExtension:
			extensionsPtr += 2 + 2 + extensionLen;
		}
nextMessage:
		i += 5 + message_length;
	}

out:
	return vrd;
}

int gen_fake_sni(struct fake_type type,
		const void *ipxh, uint32_t iph_len, 
		const struct tcphdr *tcph, uint32_t tcph_len,
		uint8_t *buf, uint32_t *buflen) {
	uint32_t data_len = type.fake_len;

	if (type.type == FAKE_PAYLOAD_RANDOM && data_len == 0) {
		data_len = (uint32_t)randint() % 1200;
	}

	if (!ipxh || !tcph || !buf || !buflen)
		return -EINVAL;

	int ipxv = netproto_version(ipxh, iph_len);

	if (ipxv == IP4VERSION) {
		const struct iphdr *iph = ipxh;

		memcpy(buf, iph, iph_len);
		struct iphdr *niph = (struct iphdr *)buf;

		niph->protocol = IPPROTO_TCP;
	} else if (ipxv == IP6VERSION) {
		const struct ip6_hdr *iph = ipxh;

		iph_len = sizeof(struct ip6_hdr);
		memcpy(buf, iph, iph_len);
		struct ip6_hdr *niph = (struct ip6_hdr *)buf;

		niph->ip6_nxt = IPPROTO_TCP;
	} else {
		return -EINVAL;
	}

	uint32_t dlen = iph_len + tcph_len + data_len;

	if (*buflen < dlen) 
		return -ENOMEM;

	memcpy(buf + iph_len, tcph, tcph_len);
	uint8_t *bfdptr = buf + iph_len + tcph_len;

	switch (type.type) {
		case FAKE_PAYLOAD_DATA:
			memcpy(bfdptr, type.fake_data, data_len);
			break;
		default: // FAKE_PAYLOAD_RANDOM
#ifdef KERNEL_SPACE
			get_random_bytes(bfdptr, data_len);
#else /* KERNEL_SPACE */
#if _NO_GETRANDOM
		{
			int ret = open("/dev/urandom", O_RDONLY);
			if (ret < 0) {
				lgerror(ret, "Unable to open /dev/urandom");
				return ret;
			}
			
			read(ret, bfdptr, data_len);
			close(ret);
		}
#else /* _NO_GETRANDOM */
			getrandom(bfdptr, data_len, 0);
#endif /* _NO_GETRANDOM */
#endif /* KERNEL_SPACE */
	}

	if (ipxv == IP4VERSION) {
		struct iphdr *niph = (struct iphdr *)buf;
		niph->tot_len = htons(dlen);
	} else if (ipxv == IP6VERSION) {
		struct ip6_hdr *niph = (struct ip6_hdr *)buf;
		niph->ip6_plen = htons(dlen - iph_len);
	}

	fail_packet(type.strategy, buf, &dlen, *buflen);

	*buflen = dlen;
	
	return 0;
}

