import struct, os
AA = bytes.fromhex("d6be898e")
def nordic_record(adva6, ad, rssi=-60, chan=38):
    # 17-byte Nordic header: [0]=04 [1]=06 [2]=paylen [3..7]... [off-8]=chan [off-7]=rssi(mag)
    hdr = bytearray(17)
    hdr[0]=0x04; hdr[1]=0x06; hdr[9]=chan & 0xff; hdr[10]=(-rssi) & 0xff
    pdu = bytes([0x02, 6+len(ad)]) + adva6 + ad        # ADV_NONCONN_IND
    payload = bytes(hdr) + AA + pdu
    hdr[2] = len(payload) - 7
    return bytes(hdr) + AA + pdu
def write_pcap(path, records):
    with open(path,"wb") as f:
        f.write(struct.pack("<IHHIIII", 0xa1b2c3d4,2,4,0,0,65535,157))
        for r in records:
            f.write(struct.pack("<IIII",0,0,len(r),len(r))); f.write(r)
def mfg(cid): return bytes([0x02,0x01,0x06, 0x04,0xFF, cid & 0xff, cid>>8, 0x11])
ADVAS=[(bytes([1,2,3,4,5,0xC0]),0x0075),(bytes([1,2,3,4,5,0xC1]),0x0075),
       (bytes([9,9,9,9,9,0xD0]),0x0087),(bytes([1,2,3,4,5,0x40]),0x004C),  # RPA
       (bytes([1,2,3,4,5,0x00]),0x0059)]                                    # public
def sample_pcap(path):
    # Nordic DLT157: 3 static-random, 1 RPA, 1 public
    write_pcap(path, [nordic_record(a, mfg(c)) for a,c in ADVAS])
def name_only(name=b"HI"):
    # flags + complete-local-name only: no 0xFF mfg data -> company parses as 0 (the "none" bucket)
    return bytes([0x02,0x01,0x06]) + bytes([1+len(name),0x09]) + name
def sample_pcap_weighted(path):
    # 3 distinct devices: chatty A (0x75, x10 adverts), B (0x87, x1), C (no-mfg, x1).
    # Advert-weighting would make 0x75 ~0.9; device-weighting makes it 1/3, none 1/3.
    A=bytes([0x11,0,0,0,0,0xC0]); B=bytes([0x22,0,0,0,0,0xC0]); C=bytes([0x33,0,0,0,0,0xC0])
    recs=[nordic_record(A, mfg(0x75)) for _ in range(10)]
    recs.append(nordic_record(B, mfg(0x87)))
    recs.append(nordic_record(C, name_only()))
    write_pcap(path, recs)
def dlt256_record(adva6, ad):
    phdr=bytes(10)                                     # 10-byte LE_LL_WITH_PHDR pseudo-header
    pdu=bytes([0x02, 6+len(ad)]) + adva6 + ad
    return phdr + AA + pdu
def sample_pcap_dlt256(path):
    with open(path,"wb") as f:
        f.write(struct.pack("<IHHIIII", 0xa1b2c3d4,2,4,0,0,65535,256))
        for a,c in ADVAS:
            r=dlt256_record(a, mfg(c)); f.write(struct.pack("<IIII",0,0,len(r),len(r))); f.write(r)
if __name__ == "__main__":
    sample_pcap(os.path.join(os.path.dirname(__file__),"sample_nordic.pcap"))
    sample_pcap_dlt256(os.path.join(os.path.dirname(__file__),"sample_dlt256.pcap"))
