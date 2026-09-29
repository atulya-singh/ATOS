#!/usr/bin/env perl
# Host side of the smoke test's network checks. QEMU's user-mode network
# shows the host's loopback to the guest as 10.0.2.2, so this listens on
# 127.0.0.1:
#   TCP <http-port>: HTTP/1.0 -- /hello.txt, /big.bin (200000 bytes: 5000
#                    numbered 40-byte lines, for bulk TCP), else 404
#   UDP <dns-port>:  DNS -- an A record 192.0.2.7 for atos.test, NXDOMAIN
#                    for anything else
# Runs until killed. Plain perl core modules only (present on CI and in
# the dev container).
use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;

my ($http_port, $dns_port) = @ARGV;
die "usage: $0 http-port dns-port\n" unless $dns_port;

my $http = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $http_port,
                                 Proto => 'tcp', Listen => 8, ReuseAddr => 1)
    or die "http listen: $!\n";
my $dns = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $dns_port,
                                Proto => 'udp')
    or die "dns bind: $!\n";

my $big = join '', map { sprintf "%-39s\n", "line $_ of the ATOS bulk TCP test" } 1 .. 5000;
my %files = ('/hello.txt' => "hello from the host\n", '/big.bin' => $big);

sub serve_http {
    my $c = $http->accept or return;
    my $req = '';
    while ($req !~ /\r?\n\r?\n/) {
        my $n = sysread($c, my $buf, 4096);
        last unless $n;
        $req .= $buf;
    }
    my ($path) = $req =~ m{^GET (\S+) HTTP/};
    my $body = defined $path ? $files{$path} : undef;
    my $resp = defined $body
        ? "HTTP/1.0 200 OK\r\nContent-Length: " . length($body) . "\r\n\r\n$body"
        : "HTTP/1.0 404 Not Found\r\nContent-Length: 10\r\n\r\nnot found\n";
    my $off = 0;
    while ($off < length $resp) {
        my $w = syswrite($c, $resp, 65536, $off);
        last unless $w;
        $off += $w;
    }
    close $c;
}

sub serve_dns {
    my $peer = $dns->recv(my $q, 512) // return;
    return if length $q < 12;
    my ($id, $flags, $qd) = unpack 'n n n', $q;
    # The question: labels up to the zero byte, then type + class.
    my $pos = 12;
    my @labels;
    while ($pos < length $q) {
        my $l = ord substr $q, $pos, 1;
        $pos++;
        last if $l == 0;
        push @labels, substr $q, $pos, $l;
        $pos += $l;
    }
    my $question = substr $q, 12, $pos + 4 - 12;
    my $name = lc join '.', @labels;
    my $found = $name eq 'atos.test';
    my $rflags = 0x8180 | ($found ? 0 : 3); # response, RD+RA, NXDOMAIN if unknown
    my $resp = pack('n n n n n n', $id, $rflags, 1, $found ? 1 : 0, 0, 0) . $question;
    # Answer: pointer to the name at offset 12, A, IN, TTL 60, 4 bytes.
    $resp .= pack('n n n N n C4', 0xC00C, 1, 1, 60, 4, 192, 0, 2, 7) if $found;
    $dns->send($resp, 0, $peer);
}

my $sel = IO::Select->new($http, $dns);
while (1) {
    for my $fh ($sel->can_read) {
        if ($fh == $http) { serve_http() } else { serve_dns() }
    }
}
