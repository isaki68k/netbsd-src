<html>
<head>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8">
<style type="text/css">
P.p {
	margin-left: 2em;
}
PRE.p {
	margin-left: 3em;
}
</style>
</head>
<body>
<h3>NetBSD audio problems</h3>
<hr>
<h4>NetBSD8 problems</h4>
<ul>

<li>sysctl channels の挙動が変? (2019/01)
<p class="p">
sysctl -w hw.*.channels で設定できない値にセットした後、
設定可能な値にセットすると前の値が見えることがある気がするが、詳細不明。
一応記録のため。
</p>
<blockquote class="p"><pre>
# sysctl -w hw.uaudio0.channels=9
invalid argument
# sysctl -w hw.uaudio0.channels=2
9->2
</pre></blockquote>

<li>uaudio(4) の trigger_{input,output} がロックを外している (2019/01)
<p class="p">
uaudio(4) の trigger_{input,output} が sc_lock, sc_intr_lock を
外してから自身の処理を行って戻る前にもう一度ロックを取り直しているが、
これは不要。というか外してはいけない。
元々 sc_lock が IPL_SCHED だった時代にはやむなくそうしていた
(しその頃の audio はシングルプレーヤーだったので問題は顕在化しなかった)が
IPL_USBSOFT に変えてからはロックを外す必要はなくなっている。
のだが、audio.c 側にバグがあるのに uaudio.c 側に適当な変更を入れている
ように見える。
当然 (1再生しかしなければ滅多に発現しないが) これでも発現する。
</p>

<li>hw_if の各メソッドの呼び出し順序の依存関係が考慮されていない、その2
(2018/05)
<p class="p">
その前に、init_output は本来バッファ長が確定後再生(録音)前に呼び出されて
バッファ長に対する処理を行うべきフックなのだが、
NetBSD7 以前の実装では(オープン後)再生(録音)開始前に一度だけ呼ばれることから、
ここで再生(録音)開始などの本来関係のないアクションを
行なっている誤ったドライバがいくつかある
(再生(録音)開始は start_output(start_input) 内で再生(録音)が始まってなければ
開始する、というようにするべきと思われる)。
具体的には melody(4)、vraiu(4)、interwave.c、vs(4) あたり。
</p>
<p class="p">
それを踏まえて、
hw_if-&gt;open と hw_if-&gt;init_output (init_input)
の間でも以下と同様のことが起きるが、
NetBSD8 では
init_output(init_input) の呼び出しタイミングが open より前に変わるので、
open でチップの初期化を行い、init_output で再生開始を行うような(正しくない)
デバイスは動かなくなりそう。
具体的には interwave.c が該当しそう。
これは合わせ技なので、お行儀の悪い MD ドライバさんサイドにも問題がある。
</p>

<li>hw_if の各メソッドの呼び出し順序の依存関係が考慮されていない
(2018/05)
<p class="p">
元々、ユーザが /dev/audio をオープンすると hw_if-&gt;open が呼ばれ、
ユーザからの ioctl(AUDIO_SETINFO) で hw_if-&gt;set_params、
hw_if-&gt;commit_settings が呼ばれて
これがハードウェアを指定のフォーマットに設定し、
それから再生なり録音を始めて最後に hw_if-&gt;close という順序だったが、
N8 ではカーネル起動時に hw_if-&gt;set_params、hw_if-&gt;commit_settings
でハードウェアを設定し、
ユーザが /dev/audio をオープンすると hw_if-&gt;open が呼ばれるという
順序に(黙って)変更になった。
このため open と set_params/commit_settings
の間に依存関係を持っていたり、
open でチップの初期化を行なっているようなデバイスは正しく動かないはず。
ぱっと見たところ、
vraiu(4)、zaudio(4)、arm/xscale/pxa2x0_ac97.c
くらいが該当しそう。
dbri(4) は該当するかどうかぱっと見不明。
</p>

<li>hw_if-&gt;close の呼び出しが適切でない (2018/05)
<p class="p">
audio_open 中でエラーが起きた際に hw_if-&gt;close を呼んでいるが、
その際規約に反して sc_lock, sc_intr_lock のロックを取っていない。
呼ばれた close が KASSERT とかでロック持ってることを確認してると死ぬ。
具体的に遭遇したのは sb(4)。
MD メソッド側でロック状態を KASSERT すること自体は珍しくないと思うけど
(調べてはない)、
オープン中にエラーが出るほうが珍しいからかもしれない。
</p>

<li>vchan.dacN/micN の表記が L のみに見える (2018/04)
<p class="p">
mixerctl -a で表示されるトラックボリュームが L, R のうち R がミュートされてる
ように読める。
実際には全チャンネルに対するトラックボリュームなので、
255,255 のように組で連動させるか、
L, R に限らないので 255 のように単独にするかしたほうがいいんじゃ。
</p>
<pre class="p">
vchan.dac0=255,0
vchan.mic0=255,0
</pre>
<p class="p">
そもそもこのネーミングやこの実装方法はどうなのかという気もしないでもないが。
</p>

<li>hw_if-&gt;open に渡すフラグが適切でない (2018/03)
<p class="p">
hw_if-&gt;open は録音と再生を足して1本目のオープンでだけコールするが、
この時の flags 引数は audioopen() の flags 引数をそのまま渡しており、これだと
この flags を見て動作を変えるような HW ドライバで
1本目で再生オープン、2本目で録音オープンのようにした場合に
再生オープンの分しか初期化が動かないようなケースがありえる。
ソース読む限り aria(4)と emuxki(4) が該当しそう。
<br>2018/06 追記。dev/ic/ad1848.c family も該当しそう。
</p>
<p class="p">
また逆に Half HW に対して open(hdl, FREAD | FWRITE) の呼び出しが失敗する
ケースは少なくとも arch/evbarm/mini2440/audio_mini2440.c がある。
Half HW に対しては必ず FREAD か FWRITE 一方ずつで呼び出さなければならない。
</p>

<li>hw_if-&gt;mappage がデッドコード (2018/03)
<p class="p">
mmap をソフトウェア側だけでの実装の変更したため、
各 MD ドライバの mappage は呼び出されなくなっている。
</p>

<li>hw_if-&gt;drain はたぶん無意味 (2018/03)
<p class="p">
hw_if-&gt;drain の呼び出しはもはや意味がなく、
ソフトウェアで判定することしか出来ないので、AUDIO2 では廃止する。
</p>

<li>オープンしただけで録音が開始される(ケースがある?) (2018/02)
<p class="p">
ただ一部それを想定していないようで、
以下の手順で eap(4) が panic する。
</p>
<pre class="p">
fd = open("/dev/sound", O_RDONLY);
AUDIO_INITINFO(&ai);
ai.record.pause = 1;
ioctl(fd, AUDIO_SETINFO, &ai);
close(fd);
fd = open("/dev/sound", O_RDONLY);
close(fd);
fd = open("/dev/sound", O_RDONLY);
</pre>

<li>AUDIO_GETINFO の ai.play.eof が VC 分離できていない (2018/02)
<p class="p">
何の意味があるかはともかく、
eof カウンタが分離されておらず全ディスクリプタで共用されている。
</p>

<li>Half Duplex で read(2) がブロックする? (2018/02)
<p class="p">
Half-Duplex HW に対して open(O_WRONLY) してwrite()と、
open(O_RDONLY) して read を同時に行うと read がブロックするようだ。
詳細未確認。
</p>

<li>Half Duplex 対応が甘い (2018/02)
<p class="p">
Half Duplex HW に対する考察が色々足りない。
元々 7 以前の時点でもちょっと微妙ではあったが、
あれはあれでシングルストリームなので一応成立はしていた。
が、そのままマルチストリームに持ち込んでも適用はできない。
特に O_RDWR オープンの挙動。
</p>

<li>sysctl のデバイス名が MD デバイス名なのはどうなのか (2018/02)
<p class="p">
sysctl hw.&lt;dev&gt; の dev は hdafg0 とか vs0 みたいな名前だが、
これと自分がオープンした /dev/audioN との対応を取るのが面倒。
drvctl 使うと分かるけども。
どうして audioN を使わないのか。
そもそも frequency、precision、channels を sysctl で設定させようという発想がおかしい。
</p>

<li>/dev/audioctl をオープンできない場合がある (2018/02)
<p class="p">
/dev/audio か /dev/sound を誰かがオープンしていると、 他の人が /dev/audioctl をオープンできないバグがある。 また、逆、つまり /dev/audioctl を誰かがオープンしていると 他の人が /dev/audio か /dev/sound をオープンできないというバグもある。
<br>
2018/06 追記。後述「gain が設定できない」件の修正パッチによって
これが副作用的に直っている可能性がある。未確認。
</p>

<li>FIOASYNC が分離できていない (2018/01)
<p class="p">
FIOASYNC が VC 分離できていないため、
非同期モードを設定しても、他の人がオフに出来る。特権に関係なく。
また FIOASYNC が最終クローズでしかクリアされないなど。
</p>

<li>gain が設定できない (2017/12)
<p class="p">
<a href="http://gnats.netbsd.org/52781">kern/52781</a> (from bouyer@)。
gain の設定は VC のソフトウェアボリュームに変更されたが、
gain の取得と balance の設定・取得が HW ミキサーのままなので不整合。
<br>
2018/06 追記。-current で修正されたようだ。詳細未確認。
</p>

<li>Speed up improvements… (2017/11)
<p class="p">
<a href="http://www.nerv.org/netbsd/changeset.cgi?id=20171128T073527Z.0bfbebe6774dd83892cfd226402bc83f19ef4ea7#src/sys/dev/audio.c">audio.c,v 1.441</a>
</p>

<li>AUDIO_SETFD で setfd を呼んではいけないはず (2017/10)
<p class="p">
ioctl の AUDIO_SETFD で、setfd を呼んで HW の Full/Half Duplex
を設定してるが、これはおかしい。
けど setfd を実装しているドライバは皆無なのでセーフセーフ?
</p>

<li>ついでに: pad(4) が色々やばい (2017/10)
<p class="p">
まだ手が回らない。
</p>

<li>ついでに: -current の pad(4) の open がメモリリークしている (2017/10)
<p class="p">
pad_open() がメモリリークしている。
<pre class="p">
while (1) {
	int fd = open("/dev/pad", O_WRONLY);
	close(fd);
}
</pre>
</p>

<li>audio beep の高精度(笑)正弦波 (2017/10)
<p class="p">
4KB の高精度データがある。貴重な1ページ消費するとか勘弁してほしい。
元は 64バイトのデータだった。
なお元実装のほうが音がきれいな模様。
</p>

<li>audio beep もたぶん色々やばそう (2017/10)
<p class="p">
ぱっと見。まだ手が回らない。
</p>

<li>/dev/speaker もたぶん色々やばそう (2017/10)
<p class="p">
ぱっと見。まだ手が回らない。
</p>

<li>AUDIO_SETCHAN/AUDIO_GETCHAN がアウト (2017/10)
<p class="p">
<a href="http://gnats.netbsd.org/52627">kern/52627</a>。
</p>

<li>15625Hz デバイスが認識されない (2017/07)
<p class="p">
プローブアルゴリズムがタコいため、15625Hz のようなデバイス(vs(4))が
アタッチできない。
</p>

<li>3ch 以上のデバイスでも 2ch としか認識されない (2017/10)
<p class="p">
3ch 以上、あるいは 48000Hz 超をサポートしているデバイスでも
2ch、48000Hz としか認識されない。
プローブアルゴリズムがタコいため。
</p>

<li>sc_lchan (2017/10)
<p class="p">
sc_lchan はもう使われていない。
</p>

<li>AUDIO_GETENC が返す集合が正しくない (2017/10)
<p class="p">
<a href="http://gnats.netbsd.org/52585">kern/52585</a>。
</p>

<li>init_input/init_output の呼び出しが過多あるいは不足 (2017/08)
<p class="p">
影響を受けるのは esm(4) だけだけど。
<s><a href="http://gnats.netbsd.org/52580">kern/52580</a></s>、
<s><a href="http://gnats.netbsd.org/52581">kern/52581</a></s>、
<s><a href="http://gnats.netbsd.org/52582">kern/52582</a></s>。
修正済み。
</p>

<li>LINEARN_TO_LINEAR マクロ (2017/09)
<p class="p">
320パターンのリニア系相互変換を行うマクロだが 80% 以上のパターンで正しく
ないし、無限ループになる組み合わせもある
→ <s><a href="http://gnats.netbsd.org/52586">kern/52586</a></s>。
一応実装自体は修正されたらしいが、
そもそもこんな相互変換おかしい。
</p>

<li>audiodetach がリソースリークしてる気がする (2017/09)
<p class="p">
<s>ちゃんと見てないけど、オープン中の VC は解放しなくていいのかな。</s>
<br>
(2018/03)
VOP_CLOSE 呼んでるのでこれで大丈夫ということか。
</p>

<li>再生開始時に前の音が一瞬再生される (2017/08)
<p class="p">
<s>
たぶん HW バッファの整合がとれてない。
速いマシンだとほぼ分からないが遅いマシンだとよく分かる。
遅いからではなく (遅いからだったとしてもだめだが)
遅マシンだと顕著に分かるのだと思う。
</s> 起きなくなってるっぽい? (2018/04)
</p>

</ul>


<hr>
<h4>NetBSD 7 (以前) からある問題</h4>
<ul>

<li>フレーム境界の考慮が不足してる気がする? (2019/01)
<p class="p">
PLAY モード (!PLAY_ALL モード)
で少なくともフィルタが設定されてない時はフレーム境界を
意識していないので、途切れる直前のフレームが不完全なものになりそう。
ただし認識できるような問題にはならなさそう。
次フレームの開始がずれることはなさげ。
記録のため。
</p>

<li>emuxki(4) の open が flags の使い方を間違えている (2018/05)
<p class="p">
<s>実害はないが emuxki(4) の open は引数 flags を AUOPEN_{READ,WRITE} と
間違えている。ここの flags で渡されるのは FREAD | FWRITE である。
ただし両者は値が同じなので実害はない。一応メモ。</s>
<br>(2019/05 修正済み)
</p>

<li>isa/aria の halt_input と halt_output が逆 (2018/04)
<p class="p">
<s>
audio(4) とは直接関係ないけど、aria(4) の halt_input と halt_output が
誤って逆になってるので、正しく停止できないはず。
ただし実際のところは hw-&gt;halt_* を呼んで停止に失敗しても、
直後に hw-&gt;close を呼ぶため、そしてこいつがチップリセットをしてるっぽい
ので、おそらく見た目に影響は分からなかったはず。
N8 でどうなるかは追っかけてないのでシラン。
</s><br>
(2018/03 修正済み)
</p>

<li>AUDIO_SETINFO.mode の挙動が分かりづらい (2018/03)
<p class="p">
<s>
mode には何がセットされていてもこの ioctl がエラーにはならない。
ただし次に AUDIO_GETINFO してみて何が取得できるかとはまったく別。
そしてここで取得できる mode はセット後の何の動作にはまったく影響しないようだ。
分かりにくすぎというかバグでは。
</s><br>(audio2 で修正済み)
</p>

<li>AUDIO_SETINFO.blocksize が manpage と実装が異なる (2018/03)
<p class="p">
<s>
audio(4) には AUDIO_SETINFO では blocksize は実際に選ばれた値が書き戻される
ように書いてあるが、実装は書き戻しを一切行なっていない。
もう manpage のほうを書き換えてしまっては…
</s><br>(audio2 で manpage を修正済み)
</p>

<li>AUDIO_SETINFO.blocksize の設定値 (と MD ドライバ) によっては panic する (2018/03)
<p class="p">
AUDIO_SETINFO で blocksize を設定するとき
まず MI ドライバが入力値を必要なら調整し、
さらに MD ドライバにおいても必要ならもう一度今度は MD ドライバで調整している。
ただし audio(9) manpage にこの round_blocksize() の制約条件が一切書かれてない
ため MD ドライバは round_blocksize() をそれぞれ好き勝手に実装しているにも関わらず、
MI は round_blocksize() の戻り値が 0 以下なら panic するようになっている。
</p>
<p class="p">
具体的にはまず MI は入力された blocksize (これの型は u_int なのだが) が
signed int で 32 未満であれば 32 とする。
そしてこの値を MD の round_blocksize() に渡して調整させるのだが、
例えば auich(4) の round_blocksize() は入力値の下位6ビットを切り捨てる
(だけの)実装になっており、つまり blocksize < 64 を指定すると 0 が返る。
そして、MI 側では round_blocksize() の戻り値が 0 なら panic する。うーんこの。
eap(4) の場合、round_blocksize() は下位5ビットを切り捨てるのだが
前述の通り
32未満の値は round_blocksize() には入力されないのでたまたま(なのか上位の動作を知っててなのか)セーフ。
</p>
<pre class="p">
// auich(4) panics
struct audio_info ai;
fd = open("/dev/audio", O_WRONLY);
AUDIO_INITINFO(&amp;ai);
ai.blocksize = 31;
ioctl(fd, AUDIO_SETINFO, &ai);
</pre>
</p>
<p class="p">
NetBSD 8 はこのあたりの実装ががっつり書き換わっているためこの問題は起きない
(別の問題がないとは言っていない、今より小さい値に変更できないように読めるので
それであれば確かにこの問題は起きない)。
</p>

<li>poll(2) で O_RDONLY に対して POLLOUT がセットされる (2018/02)
<p class="p">
O_RDONLY ディスクリプタに対して poll(POLLOUT) すると POLLOUT が返る。
当然 write(2) は出来ない。
</p>

<li>pad(4) への書き込みでプロセスがハングするケースがあるようだ (2018/01)
<p class="p">
詳細未調査。
</p>

<li>周波数 0Hz を設定するとプロセスがハングする (2017/11)
<p class="p">
これだけでプロセスが不死身になる。shutdown も効かなくなる。
<pre class="p">
struct audio_info ai;
fd = open("/dev/audio", O_WRONLY);
AUDIO_INITINFO(&amp;ai);
ai.play.sample_rate = 0;
ioctl(fd, AUDIO_SETINFO, &amp;ai);
write(fd, buf, bufsize);
</pre>
</p>

<li>mmap(2) すると次の open(2) が失敗する (2017/10)
<p class="p">
mmap されたフラグを close() でも落としていないため、
次の open 時に(フィルタの設定をしようとしたところで)こける。
でこのエラーハンドリング中に(たぶん意図せず) mmap されたフラグもクリア
しているため、その次の open は成功する。
<pre class="p">
fd = open("/dev/audio");	// success
mmap(..., fd, ...);		// success
 :
close(fd);	// success

// 2nd open
fd = open("/dev/audio");	// failure

// 3rd open
fd = open("/dev/audio");	// success
</pre>
</p>
<p class="p">
NetBSD8 はこの mmap フラグが VC ごとになって生存期間が open-close 間に
なったため、発現しない。
</p>

<li>再生中にデタッチすると死ぬ (2017/10)
<p class="p">
USB audio とかで再生中にデタッチすると死ぬ。
NetBSD 7 も同様。
</p>

</ul>

<?php
// $num .. PR 番号。負なら closed、
// 後ろに "/xxx" を付けるとカテゴリ、デフォルトは kern
// $is_audio .. MI なら true
// $title
// $comment
function item($num, $is_audio, $title, $comment = "")
{
	$closed = false;
	$category = "kern";

	if (preg_match(",([^/]+)/(.*),", $num, $m)) {
		$num = $m[1];
		$category = $m[2];
	}

	if ($num < 0) {
		$num = -$num;
		$closed = true;
	}

	print "<dt>";
	if ($is_audio)
		print "<b>";
	if ($closed)
		print "<s>";
	print "<a href=\"http://gnats.netbsd.org/${num}\">{$category}/{$num}</a>";
	if ($closed)
		print "</s><font color=#555>";
	print " {$title}";
	if ($closed)
		print "</font>";
	if ($is_audio)
		print "</b>";

	print "<dd>";
	if ($closed)
		print "<font color=#555>";
	print "{$comment}";
	if ($closed)
		print "</font>";
	print "\n";
	print "\n";
}
?>

<hr>
<h4>Related PRs</h4>
<dl>
<?php
item(3424, true,
"Audio subsystem is writeonly-hostile (and readonly-hostile)",
"単方向デバイスのサポートがいまいちだと言っているようだ。
ちなみに NetBSD 1.2 時代。");

item(6827, true,
"/dev/audio does not support mmap for recording",
"もう仕様にしては…。");

item(8479, false,
"ossaudio emulation (mis-)interprets record source MIXER_ENUMS",
"ミキサーか eso(4) の話?");

item(9087, true,
"audio mmap(2) interface not entirely clear");

item(9196, false,
"",
"ad1848 系の亜種を足してほしかったようだ");

item(14065, false,
"Record DMA not supported in the audio driver isa/wss_isa.c",
"wss(4) の録音側が不十分な件?");

item(14819, false,
"changing ym /dev/audio encoding is unreliable");

item(23571, false,
"devel/pth is unable to read audio device in various packages",
"詳細未調査。");

item(29245, false,
"kern/29245 Fix to make audio work on Alpha PWS",
"ess(4) あたりの混ぜるな危険という話?");

item(30898, false,
"auvia(4) plays audiofiles to fast and distorted",
"関係なさげ?");

item(32481, false,
"audiocs(4) mixer does not label inputs correctly, does not control output ports");

item(34821, false,
"[PATCH] Audio drivers use obsolete record gain control");

item(-35060, false,
"No audio output with azalia");

item(35465, false,
"GUS PnP audio device's still faulty");
?>


<dt><font color=#555><s>kern/36883</s> azalia device doesn't sense plugging of the headphone</font>

<?php
item(-38493, false,
"no good audio with cdplay");
?>

<dt><font color=#555><a href="http://gnats.netbsd.org/38859">kern/38859</a>
azalia record does not work on Analog Devices AD1984</font>
<dd>4.99.64 の頃から録音が動かないらしい。
おそらく MD の問題だとは思うけど。

<dt><font color=#555><a href="http://gnats.netbsd.org/40243">kern/40243</a>
audio hanging and not working</font>
<dd>auixp(4) 固有の話みたい。

<dt><font color=#555><s>kern/41927</s> azalia(4) fails on input/output on Vaio VGN-SZ340P</font>

<dt><font color=#555>kern/41957 hdaudio failure (was: azalia crashes during boot)</font>
<dt><font color=#555>kern/42055 hdaudio doesn't work on Dell Inspiron 6400</font>

<dt><a href="http://gnats.netbsd.org/42016">kern/42016</a>
audio(4) does not handle formats where validbits!=precision
<dd>24/24 と 24/32 がユーザランドで指定できないという話。

<dt><font color=#555><a href="http://gnats.netbsd.org/42028">kern/42028</a>
hdaudio: mplayer can't change volume</font>
<dd>hdaudio 固有の話?

<dt><font color=#555>kern/42315 hdaudio: Very low volume on laptop speakers on Dell XPS M1330</font>
<dt><font color=#555>kern/44654 No sound with hdaudio(4) and hdaudioctl(8) panic (Realtek ALC260)</font>
<dt><font color=#555>kern/44670 audio output doesn't work on my MacBook1,1</font>
<dt><font color=#555>kern/45477 cannot change audio volume on 5.99.55 (GENERIC)</font>

<dt><font color=#555><a href="http://gnats.netbsd.org/45862">kern/45862</a>
more hdaudio/mixer failure data in -current</font>
<dd>情報不足でよく分からないしたぶん関係ない。

<dt><font color=#555>kern/46927 audio(4) doesn't attache to HDMI hdaudio</font>

<dt><s><a href="http://gnats.netbsd.org/47077">kern/47077</a></s>
[dM] audio(4) disagrees with the code
<dd> SETINFO が書き戻しをするかどうかの話。クローズした。

<dt><font color=#555>kern/47236 hdaudio/mixer initialization badly wrong on Thinpad X61s</font>

<dt><a href="http://gnats.netbsd.org/48792">kern/48792</a>
hdaudio(4) doesn't support mmap
<dd>hdaudio が mmap をサポートしてないという PR だが、
hdaudio.c の mmap は audio driver の mmap じゃなく
character device の mmap だという別のオチはあるのだが。

<dt><font color=#555><a href="http://gnats.netbsd.org/50603">port-amd64/50603</a>
Playing audio with Linux pulseaudio apps fails on NetBSD/amd64</font>
<dd>Linux futex とかの問題のようで、audio 関係ない模様。

<dt><s><a href="http://gnats.netbsd.org/50613">kern/50613</a></s>
unpausing audio(4) causes already written data to be dropped
<dd>pause 解除するとそこまでに書き込んでたデータが消える。
N8 修正済み、N7未修正。

<dt><font color=#555><s>kern/50861</s> periodic errors from hdafg(4) on RADEON HD5450</font>

<dt><font color=#555><a href="http://gnats.netbsd.org/51734">kern/51734</a>
hdaudio errors on boot</font>
<dd>この人のデバイスでエラーが出るようだ。

<dt><s><a href="http://gnats.netbsd.org/51760">kern/51760</a></s>
audio playback sounds bad
<dd>音がひどい。直したと言っている。

<dt><font color=#555><a href="http://gnats.netbsd.org/51781">kern/51781</a>
Audio volume is changing with "inputs.dac" instead of "outputs.master"
</font>
<dd>たぶんこっちには関係なさげ。

<dt><s><a href="http://gnats.netbsd.org/51879">kern/51879</a></s>
vs(4) audio attach failed
<dd>一応解決した。正しい周波数でアタッチできない件は別途。

<dt><s><a href="http://gnats.netbsd.org/51784">kern/51784</a></s>
"synthesized" speaker hums
<dd>/dev/speaker の音が悪いという話で、
グダグダなやりとりの末にハイレゾ波形を用意して直ったような話になってるが、
たぶん問題はそこじゃない。

<dt><font color=#555><a href="http://gnats.netbsd.org/51920">kern/51920</a>
hdaudio isn't configured on TOSHIBA dynabook SS SX/15A</font>
<dd>ハードウェアドライバ固有?

<dt><s><a href="http://gnats.netbsd.org/51965">kern/51965</a></s>
audio open panics LOCKDEBUG kernel

<dt><s><a href="http://gnats.netbsd.org/51999">kern/51999</a></s>
Recent change in OSS audio breaks audio playback via pkgsrc/audio/pulseaudio
<dd>liboss 側の問題だったようだ。

<dt><s><a href="http://gnats.netbsd.org/52075">kern/52075</a></s>
/dev/audio mangles multi-track MIDI output
<dd>timidity が /dev/audio の制御に /dev/audioctl を使っていたが、
その方法はマルチトラック下では使えなくなったので timidity 側を直したという話。

<dt><s><a href="http://gnats.netbsd.org/52098">kern/52098</a></s>
audio replays buffers under load
<dd>音飛び(繰り返し)が起きるようだ。直ったと言っている。

<dt><s><a href="http://gnats.netbsd.org/52099">kern/52099</a></s>
audio crashes the kernel
<dd>ただのバグだったようだ?

<dt><s><a href="http://gnats.netbsd.org/52175">kern/52175</a></s>
audio sometimes stutters
<dd>音飛びする?。直ったと言っている。

<dt><s><a href="http://gnats.netbsd.org/52185">kern/52185</a></s>
panic: audio_init_ringbuffer: blksize=0
<dd>ブロックサイズの計算がぐちゃぐちゃな話。

<dt><s><a href="http://gnats.netbsd.org/52195">kern/52195</a></s>
segfault in audio_fill_silence()
<dd>これもブロックサイズ。

<dt><s><a href="http://gnats.netbsd.org/52196">kern/52196</a></s>
ossaudio+portaudio2 fails on HEAD
<dd>non blocking write に長いデータ書き込む話?

<dt><s><a href="http://gnats.netbsd.org/52256">kern/52256</a></s>
panic: audio_init_ringbuffer: blksize=0
<dd>再生専用デバイスの考慮が足りてなかった話。

<dt><a href="http://gnats.netbsd.org/52354">kern/52354</a>
[oss @ 0x6ce04800] Soundcard does not support 16 bit sample format
<dd>プローブ時のデフォルト precision が 32bit だったのがまずかったという話?

<dt><s><a href="http://gnats.netbsd.org/52433">kern/52433</a></s>
Dead code in audio_open()
<dd>リファクタリング。

<dt><s><a href="http://gnats.netbsd.org/52434">kern/52434</a></s>
Dead code in audio_set_params()
<dd>リファクタリング。

<dt><s><a href="http://gnats.netbsd.org/52435">kern/52435</a></s>
audio.c: mismatch of the maximum number of channels?
<dd>最大チャンネル数の定義が複数あったという話。

<dt><s><a href="http://gnats.netbsd.org/52437">kern/52437</a></s>
Refactoring audio_set_vchan_defaults()
<dd>リファクタリング。

<dt><s><a href="http://gnats.netbsd.org/52442">kern/52442</a></s>
x68k's vs(4) is broken after in-kernel mixing
<dd>vs(4) がいろいろおかしい件。

<dt><s><a href="http://gnats.netbsd.org/52459">kern/52459</a></s>
audio_fill_silence() stops playback.
<dd>再生処理が間に合わないケースが考慮されてなかったという話。

<dt><font color=#555><a href="http://gnats.netbsd.org/52521">kern/52521</a>
No sound from my Dell 2-in-1 machine</a></font>
<dd>ハードウェアか HW ミキサーの設定の問題?

<dt><font color=#555><a href="http://gnats.netbsd.org/52540">kern/52540</a></s>
auich: The volume is maximum? on initial audio playback</font>
<dd>ハードウェアドライバ側の問題。

<dt><s><a href="http://gnats.netbsd.org/52580">kern/52580</a></s>
audio: init_output/input called multiple times per open
<dd> 上述

<dt><s><a href="http://gnats.netbsd.org/52581">kern/52581</a></s>
audio: open(O_RDONLY) calls init_output
<dd>上述

<dt><s><a href="http://gnats.netbsd.org/52582">kern/52582</a></s>
audio: init_input is not called under certain situation
<dd>上述

<dt><s><a href="http://gnats.netbsd.org/52585">kern/52585</a></s>
AUDIO_GETENC does not return actual encodings
<dd>仕様を考えずに動作を変更している。

<dt><s><a href="http://gnats.netbsd.org/52627">kern/52627</a></s>
ioctl(AUDIO_SETCHAN) is able to affect privileged process
<dd>上述

<dt><s><a href="http://gnats.netbsd.org/52685">kern/52685</a></s>
audio stutters
<dd>音飛びする?

<dt><s><a href="http://gnats.netbsd.org/52781">kern/52781</a></s>
audioctl can't set output gain
<dd>上述

<?php
item(-52889, false,
"amd64 panic during audio tests",
"pad デタッチ中に死ぬ。
そもそもスレッド駆動してるからなのと、オンデマンドアタッチしてるのも問題なのでは。");

item(52912, false,
"hdafg: no sound from jack port on a laptop",
"ハードウェアドライバ固有の問題?");

item(-53028, false,
"hdaudio default latency too high, mpv spins at 100% CPU playing audio",
"レイテンシが高すぎるという話と、s32 再生で CPU 100% になるという話。");

item(-53029, false,
"src/sys/dev/hdaudio/hdafg.c:1267: dead code block ?",
"リファクタリング。");

item(53230, false,
"dwc2 crashes from uaudio",
"録音中にロックで死ぬ話。");

item(-53802, false,
"audioctl not working for usb audio dev",
"outputs.master を持たないデバイスに対しては audioctl play.gain が効かないという話。
play.gain は outputs.master につながっているのでどうしたもんか。");

item(54005, false,
"X server vt switching stops audio",
"");

print "<hr width=80%>\n";
print "<dt>太字は MI 関連(か自分担当)\n<dd><p>\n";

item(-54177, true,
"playing audio in firefox doesn't work after kernel update",
"file lock がきつすぎた件。");

item(-54186, true,
"pad(4) audio tests fail",
"padのテストが (sparc で) こける。次の PR にマージした。");

item(-54187, true,
"dev/audio/t_pad:pad_output test now fails",
"padのテストが (i386/amd64 で) こける。");

item(-54229, true,
"audio in firefox stops playing",
"/dev/sound の pause が他人に影響する話。");

item(-54230, false,
"mpv drops video frames with `--ao=oss' (probably after isaki-audio2 merge)",
"mpv+oss でフレームドロップする話。");

item(54243, true,
"beeping woes with -current",
"beepがうるさい(?)という話?");

item("-54245/xsrc", false,
"xset(1): bell duration 0 is infinite",
"xset b * * 0 でビープが無限(実際にはすごく長い値)になるという話。
wscons 側の問題。");

item(-54264, false,
"audio(4) API fails to detect a capture-only device",
"uaudio(4) の get_props() が正しくなかった問題。");

item(-54282, true,
"kernel panic when 'sysctl hw.audio0'",
"sysctl_teardown() が原因だったっぽい。");

item(-54427, true,
"panic in audio_close",
"pad を先にクローズすると死ぬ問題。");

item(-54474, true,
"Jetson TK1 audio playback has clicks since isaki-audio2 merge",
"hdafg_round_blocksize の問題。");

item(54547, false,
"running stress-ng --dev 1 will cause the kernel to panic and reboot",
"詳細不明。nat が N8 の mmap あたりを直したようだ。");

item("-54614/port-arm", false,
"playing with uaudio panics on rpi",
"dwc2 の問題。");

item(-54658, false,
"zaudio(4) attach failure (after MI i2c(4) changes?)",
"MI i2c 側の問題。");

item(-54662, true,
"uaudio sometimes not recognized",
"メンバ変数の初期化漏れ。他に同様のドライバはなかった。");

item(-54667, false,
"libossaudio returns incorrect recording sample rate",
"録音再生が分離されたことに libossaudio 側が対応してなかった件。");

item("-54696/port-evbarm", false,
"Kernel panic in bus_dma.c on Raspberry Pi 3B/3B+",
"dwc2 の問題。");

item(54700, true,
"beeps through audio device stop happening",
"ビープがとまる場合があるらしい?");

item(54705, false,
"uaudio: move computing of the number of input channels under UAUDIO_DEBUG",
"ifdef の問題だが元コードで問題が再現しない。");

item(-54796, true,
"9.0_RC1 audio(4) malloc failed",
"M_NOWAIT の件。");

item(54917, false,
"pad(4) has a memory leak",
"pad open が cf を解放してない件。");

item(-54973, true,
"audio regression with multi channel content and stereo hardware",
"2ch ハードで libossaudio 経由の 5.1ch が 2ch(L,R) にshrink される話。");

item(-55017, true,
"audio0 autoconfiguration error on auvia",
"auvia(4) の blocksize 制約が厳しすぎる件。");

item(55130, false,
"audio sometimes plays a buzz sound for a few seconds when changing volume",
"たぶん RealTek hdafg(4) の問題。");

item(-55175, false,	/* it's not mine */
"Interpreation of AUDIO_FORMAT_LINEAR is wrong for 8-bit samples",
"nia が勝手に commit したので放棄。");

item(55301, true,
"rare audio panic on resume, assertion \"sc->sc_pbusy == false\" failed",
"nia が勝手に変更したがその後書き直した。");

item(-55507, false,
"sometimes hdaudio panics on attach, possible memory corruption",
"hdafg(4) が起動中にパニックした?");

item(-55848, true,
"amd64 9/99.76 panic in audio(4)",
"audio_open() でエラー時に rmixer を停止してなかった件。");

item(55856, false,
"uaudio(4) device timeout on C-Media USB sound  card",
"trigger_output で EIO になるデバイスがあるようだが xhci(4) の問題らしい。");

item(55876, false,
"sparc tests hang at lib/libossaudio/t_ossaudio",
"dspのほうは GETBUFINFO の戻り値を SET に使えない件。
それをパスした後 read/write は qemu で audiocs(4) が動いてない問題。");

item(55878, false,
"Doc error in audioctl(1)",
"書いてあることが違う?、たぶんドキュメントを直すべき");

item(-56059, true,
"speaker(4): system hangs if tone < 100 Hz (_SC_CLK_TCK) is played",
"audiobell のバッファ長計算の問題。");

item(-56060, true,
"speaker(4) does not produce silence with 'P'",
"spkr まわりのパッチが未 commit のまま残ってた問題。");

item(56073, false,
"-",
"ちょっと何言ってるか分からない。");

/*...*/

item(56518, false,
"USB panic when using uaudio",
"たぶん uaudio 側");

item(56581, false,
"audiocfg: Kernel panic on USB audio test use",
"たぶん usb 側");

item(-56644, true,
"Kernel assertion in audio.c",
"audio_read() が track_recor() を呼び出す条件を修正");

item(56659, false,
"Focusrite Scarlett 2i4 USB audio autoconfiguration error",
"たぶん uaudio 側");

item(56660, false,
"Douk Audio U2 PRO USB (XMOS HIFI DSD) audio autoconfiguration error",
"たぶん uaudio 側");

item(56738, false,
"ukbd(4): PMFE_AUDIO_VOLUE* event issue, Xorg KeyPress events ...",
"キー側の話");

item(-56947, true,
"audio(4) may fail uobj allocation under VA fragmentation/preassure",
"malloc まわりを色々修正してみた");

item(56980, true,
"The Sound Blaster Audigy Rx is not supported by NetBSD",
"emuxki(4) への追加パッチのようだが、まだ動かないようだ?");

item(57030, false,
"pinebook:Can't see audio interface.aiomixer do nothing.",
"");

item(57087, false,
"audio from speaker not working",
"hdafg(4)?");


item(57322, false,
"hdafg(4) hotplug switch detection races with suspend/resume and detach");

item(57612, false,
"Using audio locks up Shark");

item(-57890, false,
"uvm_fault followed by: fatal page fault in supervisor mode, right before reboot - amd64/10.0_RC3",
"");

item(58031, false,
"audio keys don't work on thinkpad X200",
"pckbd(4) 側の話");

item("58181/port-amd64", false,
"wsbell causes system to freeze on VE-900",
"hdafg(4) の問題");

?>

</dl>

</body>
</html>
