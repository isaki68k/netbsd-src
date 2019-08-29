#
# This file is re-generated from t_audio.awk
#

# Call real test program, and then dispatch the result for atf.
h_audio() {
	local testname=$1
	outmsg=`$(atf_get_srcdir)/audiotest -AR $testname`
	local retval=$?
	if [ "$retval" = "0" ]; then
		atf_pass
	elif [ "$retval" = "1" ]; then
		atf_fail "$outmsg"
	elif [ "$retval" = "2" ]; then
		atf_skip "$outmsg"
	else
		atf_fail "unknown error $retval"
	fi
}

atf_test_case open_mode_RDONLY
open_mode_RDONLY_head() {
}
open_mode_RDONLY_body() {
	h_audio open_mode_RDONLY
}

atf_test_case open_mode_WRONLY
open_mode_WRONLY_head() {
}
open_mode_WRONLY_body() {
	h_audio open_mode_WRONLY
}

atf_test_case open_mode_RDWR
open_mode_RDWR_head() {
}
open_mode_RDWR_body() {
	h_audio open_mode_RDWR
}

atf_test_case open_audio_RDONLY
open_audio_RDONLY_head() {
}
open_audio_RDONLY_body() {
	h_audio open_audio_RDONLY
}

atf_test_case open_audio_WRONLY
open_audio_WRONLY_head() {
}
open_audio_WRONLY_body() {
	h_audio open_audio_WRONLY
}

atf_test_case open_audio_RDWR
open_audio_RDWR_head() {
}
open_audio_RDWR_body() {
	h_audio open_audio_RDWR
}

atf_test_case open_sound_RDONLY
open_sound_RDONLY_head() {
}
open_sound_RDONLY_body() {
	h_audio open_sound_RDONLY
}

atf_test_case open_sound_WRONLY
open_sound_WRONLY_head() {
}
open_sound_WRONLY_body() {
	h_audio open_sound_WRONLY
}

atf_test_case open_sound_RDWR
open_sound_RDWR_head() {
}
open_sound_RDWR_body() {
	h_audio open_sound_RDWR
}

atf_test_case open_sound_sticky
open_sound_sticky_head() {
}
open_sound_sticky_body() {
	h_audio open_sound_sticky
}

atf_test_case open_simul_RDONLY_RDONLY
open_simul_RDONLY_RDONLY_head() {
}
open_simul_RDONLY_RDONLY_body() {
	h_audio open_simul_RDONLY_RDONLY
}

atf_test_case open_simul_RDONLY_WRONLY
open_simul_RDONLY_WRONLY_head() {
}
open_simul_RDONLY_WRONLY_body() {
	h_audio open_simul_RDONLY_WRONLY
}

atf_test_case open_simul_RDONLY_RDWR
open_simul_RDONLY_RDWR_head() {
}
open_simul_RDONLY_RDWR_body() {
	h_audio open_simul_RDONLY_RDWR
}

atf_test_case open_simul_WRONLY_RDONLY
open_simul_WRONLY_RDONLY_head() {
}
open_simul_WRONLY_RDONLY_body() {
	h_audio open_simul_WRONLY_RDONLY
}

atf_test_case open_simul_WRONLY_WRONLY
open_simul_WRONLY_WRONLY_head() {
}
open_simul_WRONLY_WRONLY_body() {
	h_audio open_simul_WRONLY_WRONLY
}

atf_test_case open_simul_WRONLY_RDWR
open_simul_WRONLY_RDWR_head() {
}
open_simul_WRONLY_RDWR_body() {
	h_audio open_simul_WRONLY_RDWR
}

atf_test_case open_simul_RDWR_RDONLY
open_simul_RDWR_RDONLY_head() {
}
open_simul_RDWR_RDONLY_body() {
	h_audio open_simul_RDWR_RDONLY
}

atf_test_case open_simul_RDWR_WRONLY
open_simul_RDWR_WRONLY_head() {
}
open_simul_RDWR_WRONLY_body() {
	h_audio open_simul_RDWR_WRONLY
}

atf_test_case open_simul_RDWR_RDWR
open_simul_RDWR_RDWR_head() {
}
open_simul_RDWR_RDWR_body() {
	h_audio open_simul_RDWR_RDWR
}

atf_test_case open_multiuser_0
open_multiuser_0_head() {
}
open_multiuser_0_body() {
	h_audio open_multiuser_0
}

atf_test_case open_multiuser_1
open_multiuser_1_head() {
}
open_multiuser_1_body() {
	h_audio open_multiuser_1
}

atf_test_case write_PLAY_ALL
write_PLAY_ALL_head() {
}
write_PLAY_ALL_body() {
	h_audio write_PLAY_ALL
}

atf_test_case write_PLAY
write_PLAY_head() {
}
write_PLAY_body() {
	h_audio write_PLAY
}

atf_test_case read
read_head() {
}
read_body() {
	h_audio read
}

atf_test_case rdwr_fallback_RDONLY
rdwr_fallback_RDONLY_head() {
}
rdwr_fallback_RDONLY_body() {
	h_audio rdwr_fallback_RDONLY
}

atf_test_case rdwr_fallback_WRONLY
rdwr_fallback_WRONLY_head() {
}
rdwr_fallback_WRONLY_body() {
	h_audio rdwr_fallback_WRONLY
}

atf_test_case rdwr_fallback_RDWR
rdwr_fallback_RDWR_head() {
}
rdwr_fallback_RDWR_body() {
	h_audio rdwr_fallback_RDWR
}

atf_test_case drain_onrec
drain_onrec_head() {
}
drain_onrec_body() {
	h_audio drain_onrec
}

atf_init_test_cases() {
	atf_add_test_case open_mode_RDONLY
	atf_add_test_case open_mode_WRONLY
	atf_add_test_case open_mode_RDWR
	atf_add_test_case open_audio_RDONLY
	atf_add_test_case open_audio_WRONLY
	atf_add_test_case open_audio_RDWR
	atf_add_test_case open_sound_RDONLY
	atf_add_test_case open_sound_WRONLY
	atf_add_test_case open_sound_RDWR
	atf_add_test_case open_sound_sticky
	atf_add_test_case open_simul_RDONLY_RDONLY
	atf_add_test_case open_simul_RDONLY_WRONLY
	atf_add_test_case open_simul_RDONLY_RDWR
	atf_add_test_case open_simul_WRONLY_RDONLY
	atf_add_test_case open_simul_WRONLY_WRONLY
	atf_add_test_case open_simul_WRONLY_RDWR
	atf_add_test_case open_simul_RDWR_RDONLY
	atf_add_test_case open_simul_RDWR_WRONLY
	atf_add_test_case open_simul_RDWR_RDWR
	atf_add_test_case open_multiuser_0
	atf_add_test_case open_multiuser_1
	atf_add_test_case write_PLAY_ALL
	atf_add_test_case write_PLAY
	atf_add_test_case read
	atf_add_test_case rdwr_fallback_RDONLY
	atf_add_test_case rdwr_fallback_WRONLY
	atf_add_test_case rdwr_fallback_RDWR
	atf_add_test_case drain_onrec
}
