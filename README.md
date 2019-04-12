AUDIO2
=====

This is an alternative implementation of NetBSD audio in-kernel mixer.
But not completed yet :-)

## Description

NetBSD8 audio は in-kernel mixer をサポートしているが、
いろいろ問題が多すぎてとてもじゃないレベルだったので
一から書いてみた。
書き起こしついでにパフォーマンスも改善。

~~<font color="#808080">ところで、OpenBSD は同様のミキシングを
カーネルではなくユーザランドで行っていてそれはそれでとても合理てk…</font>~~


## NetBSD8 problems

* 音飛びするなど再生クオリティが悪い (特に遅いマシンで顕著)。
* kernel thread のため負荷が高い (特に遅いマシンで顕著)。
* 従来の MI-MD インタフェースでは hardware の周波数特性を取得できないため
(in-kernel mixer で顕在化したため)、
vs(4) などの珍しい周波数の hardware がサポートできない。
* Hi-res audio / multichannel デバイスをアタッチできない
(起動後に手動で切り替える仕組み)。
* 再生専用デバイス、録音専用デバイスについての検討が不十分。
* 多重化にあたり Half duplex device についての検討が不十分。
* in-kernel mixer 導入により不要になったはずのコードが山ほど残っている。
* 実装バグが多い。
* 仕様バグが多い。

つまりここは
「速いマシンで 48k/2ch Full duplex device でオーディオ再生を多重化する
(録音なんて滅多にしない)」という一本道だけ啓開された広大な地雷原なのだよ。
<font color="#808080">(CV:中田譲治)</font>


## AUDIO2 Features

* interrupt based system.
* Improve performance.  See below.
* Improve stability.  x68k でも音飛びは起きない。
* おそらく latency も改善しているはず。
* MI-MD インタフェースをいくつか変更。
これにより vs(4) が正しい周波数で動作できる (俺的に重要)。
また Hi-res audio / multichannel デバイスも直接アタッチ可能
* Half duplex device についてはいくつか仕様変更などを行っている。
* Fix many NetBSD8 problems.
* Fix some NetBSD7 or earlier problems.
* 単純化とパフォーマンス改善のため、NetBSD2 から導入されている
万能 stream filter を廃止して、新しい変換機構を導入。
* 自動テストを少しは頑張っている。


## Supported devices
|Device	|Backend|(port)	|query_format|set_format|Status
|-------|-------|-------|-------|-------|-------|
|arcofi(4)	|	|hp300	|Done	|Done	|Work
|audioamd(4)	|am7930	|sparc	|Done	|Done	|Compiled but not tested
|auich(4)	|	|	|Done	|Done	|Work
|autri(4)	|	|	|	|	|Work
|aucc(4)	|	|amiga	|Done	|Done	|Compiled but not tested
|bba(4)		|am7930	|alpha	|Done	|Done	|Work
|eap(4)		|	|	|Done	|modify set_params|Work
|gus(4)@isapnp	|	|	|Done	|Done	|Compiled but not tested
|harmony(4)	|	|hppa	|Done	|Done	|Compiled but not tested
|hdafg(4)	|	|	|Done	|	|Work
|melody(4)	|	|amiga	|	|	|Compiled but not tested
|mercury(4)	|	|x68k	|Done	|Done	|Added, Work (*)
|pad(4)		|	|	|Done	|Done	|Work
|psgpam(4)	|	|luna68k|Done	|Done	|Added, Work (*)
|repulse(4)	|	|amiga	|Done	|Done	|Compiled but not tested
|sb(4)		|	|	|	|modify set_params|Work as half duplex (*)
|uaudio(4)	|	|	|Done	|	|Work
|vs(4)		|	|x68k	|Done	|Done	|Work (*)
|vcaudio(4)	|	|evbarm/RPI	|Done	|Done	|Compiled but not tested
|vidcaudio(4)	|	|acorn32|Done	|Done	|Compiled but not tested
|vsaudio(4)	|am7930	|vax	|Done	|Done	|Work
|wss(4)		|	|	|	|	|Work
|yds(4)		|	|	|Done	|Done	|Work

Note:
* mercury(4) は AUDIO2 で初サポート。
* psgpam(4) は AUDIO2 で初サポート。
周波数特性上 AUDIO2 でないと正しくサポートできない。
* sb(4) は full duplex モデルもあるが、その実現方法がちょっとイレギュラー
なので、今の所 half duplex デバイスとして実装してある。
* vs(4) は周波数特性上 AUDIO2 でないと正しくサポートできない。


## Files

* sys/dev/audio/audio.c … main routines (alternative to sys/dev/audio.c)
* sys/dev/audio/audiodef.h … header file for audio.c (いるかなこれ?)
* sys/dev/audio/audiovar.h … alternative to sys/dev/audiovar.h
* sys/dev/audio/alaw.c … alaw conversion (alternative to sys/dev/mulaw.c)
* sys/dev/audio/linear.[ch] … linear-to-linear conversion
* sys/dev/audio/mulaw.[ch] … mulaw conversion (alternative to sys/dev/mulaw.[ch])


## Build

ビルド方法は基本いつもの準拠。
カーネルは amd64/conf/AUDIO2、x68k/conf/AUDIO2。
GENERIC など非 AUDIO2 カーネルもなるべくビルドできるとは思うけど、
そっちは本腰入れて維持してないので知らん。

```
% git clone --branch audio2 git@github.com:isaki68k/netbsd-src.git
% cd netbsd-src/sys/arch/amd64/conf
% config AUDIO2
% cd ../compile/AUDIO2
% make depend && make
```

## Benchmark

2018/04 現在。
今時のマシンは十分速いのでパフォーマンスの差を体感するのは無理だと思うが、
x68k くらいになると有意な差が出る。
以下は
x68k (X68030, 68030/30MHz, memory 12MB) で
vmstat -w 1 から 100 - average(idle) で CPU load を計算したもので、
厳密ではないだろうけど、とりあえず傾向としては十分だろう。

| |NetBSD7	|NetBSD8	| AUDIO2
|---|---|---|---|
|playing single mulaw/8000Hz/1ch	|4% (*1)	| 67% (*2)	| 9% (*3)
|playing single s16/22050Hz/1ch		|NotSupported (*4)| 50% (*5)	| 10% (*6)

 *1: 8000Hz を 7813Hz モードで再生
 *2: 8000Hz を 16000Hz に変換して 15625Hz モードで再生
 *3: 8000Hz を 15625Hz に変換して 15625Hz モードで再生
 *4: HW の再生可能周波数を越えており、NetBSD7 の vs(4) は周波数変換を行わない
ため、再生不可
 *5: 22050Hz を 16000Hz に変換して 15625Hz モードで再生
 *6: 22050Hz を 15625Hz に変換して 15625Hz モードで再生

メモリ使用量とかも改善してるつもりだけど、調べてない。
