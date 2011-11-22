/*!
 * \file gps_l1_ca_dll_pll_tracking_cc.cc
 * \brief code DLL + carrier PLL
 * \author Carlos Aviles, 2010. carlos.avilesr(at)googlemail.com
 *         Javier Arribas, 2011. jarribas(at)cttc.es
 *
 * Code DLL + carrier PLL according to the algorithms described in [1]
 * [1] K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency Approach, Birkha user, 2007
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2011  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "gps_l1_ca_dll_pll_tracking_cc.h"
#include "gps_sdr_signal_processing.h"
#include "tracking_discriminators.h"
#include "CN_estimators.h"
#include "GPS_L1_CA.h"

#include "control_message_factory.h"
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <sstream>
#include <cmath>
#include "math.h"

#include <gnuradio/gr_io_signature.h>

#include <glog/log_severity.h>
#include <glog/logging.h>

/*!
 * \todo Include in definition header file
 */
#define CN0_ESTIMATION_SAMPLES 10

using google::LogMessage;

gps_l1_ca_dll_pll_tracking_cc_sptr
gps_l1_ca_dll_pll_make_tracking_cc(unsigned int satellite, long if_freq, long fs_in, unsigned
			int vector_length, gr_msg_queue_sptr queue, bool dump, float pll_bw_hz, float dll_bw_hz, float early_late_space_chips) {

	return gps_l1_ca_dll_pll_tracking_cc_sptr(new gps_l1_ca_dll_pll_tracking_cc(satellite, if_freq,
			fs_in, vector_length, queue, dump, pll_bw_hz, dll_bw_hz, early_late_space_chips));
}

void gps_l1_ca_dll_pll_tracking_cc::forecast (int noutput_items,
    gr_vector_int &ninput_items_required){
    ninput_items_required[0] =(int)d_vector_length*2; //set the required available samples in each call
}

gps_l1_ca_dll_pll_tracking_cc::gps_l1_ca_dll_pll_tracking_cc(unsigned int satellite, long if_freq, long fs_in, unsigned
		int vector_length, gr_msg_queue_sptr queue, bool dump, float pll_bw_hz, float dll_bw_hz, float early_late_space_chips) :
	    gr_block ("gps_l1_ca_dll_pll_tracking_cc", gr_make_io_signature (1, 1, sizeof(gr_complex)),
	              gr_make_io_signature(5, 5, sizeof(float))) {

		//gr_sync_decimator ("gps_l1_ca_dll_pll_tracking_cc", gr_make_io_signature (1, 1, sizeof(gr_complex)),
		//		gr_make_io_signature(3, 3, sizeof(float)),vector_length) {
	// initialize internal vars
	d_queue = queue;
	d_dump = dump;
	d_satellite = satellite;
	d_if_freq = if_freq;
	d_fs_in = fs_in;
	d_vector_length = vector_length;

	//std::cout<<"pll_bw_hz= "<<pll_bw_hz<<"dll_bw_hz="<<dll_bw_hz<<"\r\n";

	// Initialize tracking  ==========================================

	d_code_loop_filter.set_DLL_BW(dll_bw_hz);
	d_carrier_loop_filter.set_PLL_BW(pll_bw_hz);

	//--- DLL variables --------------------------------------------------------
	d_early_late_spc_chips = early_late_space_chips; // Define early-late offset (in chips)

	// Initialization of local code replica
    // Get space for a vector with the C/A code replica sampled 1x/chip
    d_ca_code=new gr_complex[(int)GPS_L1_CA_CODE_LENGTH_CHIPS+2];

    // Get space for the resampled early / prompt / late local replicas
    d_early_code= new gr_complex[d_vector_length*2];
    d_prompt_code=new gr_complex[d_vector_length*2];
    d_late_code=new gr_complex[d_vector_length*2];

    // space for carrier wipeoff and signal baseband vectors
    d_carr_sign=new gr_complex[d_vector_length*2];

    //--- Perform initializations ------------------------------
    // define initial code frequency basis of NCO
    d_code_freq_hz      = GPS_L1_CA_CODE_RATE_HZ;
    // define residual code phase (in chips)
    d_rem_code_phase_samples  = 0.0;
    // define residual carrier phase
    d_rem_carr_phase_rad  = 0.0;

    // sample synchronization
    d_sample_counter=0;
    d_acq_sample_stamp=0;

    d_enable_tracking=false;
    d_pull_in=false;
    d_last_seg=0;

    d_current_prn_length_samples=(int)d_vector_length;

    // CN0 estimation and lock detector buffers
    d_cn0_estimation_counter=0;
    d_Prompt_buffer=new gr_complex[CN0_ESTIMATION_SAMPLES];
    d_carrier_lock_test=1;
    d_CN0_SNV_dB_Hz=0;
    d_carrier_lock_fail_counter=0;
    d_carrier_lock_threshold=5;
}

void gps_l1_ca_dll_pll_tracking_cc::start_tracking(){
	/*!
	 *  correct the code phase according to the delay between acq and trk
	 */
	unsigned long int acq_trk_diff_samples;
	float acq_trk_diff_seconds;
	acq_trk_diff_samples=d_sample_counter-d_acq_sample_stamp-d_vector_length;
	acq_trk_diff_seconds=acq_trk_diff_samples/(float)d_fs_in;
	//doppler effect
	// Fd=(C/(C+Vr))*F
	float radial_velocity;
	radial_velocity=(GPS_L1_FREQ_HZ+d_acq_carrier_doppler_hz)/GPS_L1_FREQ_HZ;
	// new chip and prn sequence periods based on acq Doppler
	float T_chip_mod_seconds;
	float T_prn_mod_seconds;
	float T_prn_mod_samples;
	d_code_freq_hz=radial_velocity*GPS_L1_CA_CODE_RATE_HZ;
	T_chip_mod_seconds=1/d_code_freq_hz;
	T_prn_mod_seconds=T_chip_mod_seconds*GPS_L1_CA_CODE_LENGTH_CHIPS;
	T_prn_mod_samples=T_prn_mod_seconds*(float)d_fs_in;

    d_code_phase_step_chips = d_code_freq_hz / (float)d_fs_in; //[chips]
    d_next_prn_length_samples=round(T_prn_mod_samples);
	//compute the code phase chips prediction
	float delta_T_prn_samples;
	float delay_correction_samples;
	delta_T_prn_samples=fmod((float)acq_trk_diff_samples,T_prn_mod_samples);
	delay_correction_samples=T_prn_mod_samples-delta_T_prn_samples;
	d_acq_code_phase_samples=d_acq_code_phase_samples-delay_correction_samples;
	if (d_acq_code_phase_samples<0){
		d_acq_code_phase_samples=d_acq_code_phase_samples+T_prn_mod_samples;
	}
	d_carrier_doppler_hz=d_acq_carrier_doppler_hz;
	// DLL/PLL filter initialization
	d_carrier_loop_filter.initialize(d_carrier_doppler_hz); //initialize the carrier filter
	d_code_loop_filter.initialize(d_acq_code_phase_samples); //initialize the code filter

	// generate local reference ALWAYS starting at chip 1 (1 sample per chip)
    code_gen_conplex(&d_ca_code[1],d_satellite,0);
    d_ca_code[0]=d_ca_code[(int)GPS_L1_CA_CODE_LENGTH_CHIPS];
    d_ca_code[(int)GPS_L1_CA_CODE_LENGTH_CHIPS+1]=d_ca_code[1];

	d_carrier_lock_fail_counter=0;
	d_rem_code_phase_samples=0;
	d_next_rem_code_phase_samples=0;
	d_rem_carr_phase_rad=0;
	d_acc_carrier_phase_rad=0;

	// ############# ENABLE DATA FILE LOG #################
	if (d_dump==true)
	{
		if (d_dump_file.is_open()==false)
		{
			try {
				d_dump_filename="track_ch"; //base path and name for the tracking log file
				d_dump_filename.append(boost::lexical_cast<std::string>(d_channel));
				d_dump_filename.append(".dat");
				d_dump_file.exceptions ( std::ifstream::failbit | std::ifstream::badbit );
				d_dump_file.open(d_dump_filename.c_str(), std::ios::out | std::ios::binary);
				std::cout<<"Tracking dump enabled on channel "<<d_channel<<" Log file: "<<d_dump_filename.c_str()<<std::endl;
			}
			catch (std::ifstream::failure e) {
				std::cout << "channel "<<d_channel <<" Exception opening trk dump file "<<e.what()<<"\r\n";
			}
		}
	}
	// DEBUG OUTPUT
	std::cout<<"Tracking start on channel "<<d_channel<<" for satellite ID* "<< this->d_satellite<< std::endl;
	DLOG(INFO) << "Start tracking for satellite "<<this->d_satellite<<" received ";
	// enable tracking
	d_pull_in=true;
	d_enable_tracking=true;
	std::cout<<"PULL-IN Doppler [Hz]= "<<d_carrier_doppler_hz<<" PULL-IN Code Phase [samples]= "<<d_acq_code_phase_samples<<"\r\n";
}

void gps_l1_ca_dll_pll_tracking_cc::update_local_code()
{
	float tcode_chips;
	float rem_code_phase_chips;
	int associated_chip_index;
	int code_length_chips=(int)GPS_L1_CA_CODE_LENGTH_CHIPS;
	// unified loop for E, P, L code vectors
	rem_code_phase_chips=d_rem_code_phase_samples*(d_code_freq_hz/d_fs_in);
	tcode_chips=-rem_code_phase_chips;
    for (int i=0;i<d_current_prn_length_samples;i++)
    {
        associated_chip_index=1+round(fmod(tcode_chips-d_early_late_spc_chips,code_length_chips));
        d_early_code[i] = d_ca_code[associated_chip_index];
        associated_chip_index = 1+round(fmod(tcode_chips, code_length_chips));
        d_prompt_code[i] = d_ca_code[associated_chip_index];
        associated_chip_index = 1+round(fmod(tcode_chips+d_early_late_spc_chips, code_length_chips));
        d_late_code[i] = d_ca_code[associated_chip_index];
        tcode_chips=tcode_chips+d_code_phase_step_chips;
    }
}

void gps_l1_ca_dll_pll_tracking_cc::update_local_carrier()
{
        float phase_rad, phase_step_rad;

        phase_step_rad = (float)TWO_PI*d_carrier_doppler_hz/d_fs_in;
        phase_rad=d_rem_carr_phase_rad;
        for(int i = 0; i < d_current_prn_length_samples; i++) {
            d_carr_sign[i] = gr_complex(cos(phase_rad),sin(phase_rad));
            phase_rad += phase_step_rad;
        }
        d_rem_carr_phase_rad=fmod(phase_rad,TWO_PI);
        d_acc_carrier_phase_rad=d_acc_carrier_phase_rad+d_rem_carr_phase_rad;
}

gps_l1_ca_dll_pll_tracking_cc::~gps_l1_ca_dll_pll_tracking_cc() {
	d_dump_file.close();
    delete d_ca_code;
    delete d_early_code;
    delete d_prompt_code;
    delete d_late_code;
    delete d_carr_sign;
    delete d_Prompt_buffer;
}

/*! Tracking signal processing
 * Notice that this is a class derived from gr_sync_decimator, so each of the ninput_items has vector_length samples
 */

int gps_l1_ca_dll_pll_tracking_cc::general_work (int noutput_items, gr_vector_int &ninput_items,
    gr_vector_const_void_star &input_items, gr_vector_void_star &output_items) {
	if (d_enable_tracking==true){
	    if (d_pull_in==true)
	    {
	        int samples_offset=ceil(d_acq_code_phase_samples);
	        consume_each(d_acq_code_phase_samples); //shift input to perform alignement with local replica
	        d_sample_counter+=samples_offset; //count for the processed samples
	        d_pull_in=false;
	        return 1;
	    }

	    d_current_prn_length_samples=d_next_prn_length_samples;

		float carr_error;
		float carr_nco;
		float code_error;
		float code_nco;

		const gr_complex* in = (gr_complex*) input_items[0]; //PRN start block alignement
		float **out = (float **) &output_items[0];

		update_local_code();
		update_local_carrier();

		gr_complex bb_signal_sample(0,0);
		d_Early=gr_complex(0,0);
		d_Prompt=gr_complex(0,0);
		d_Late=gr_complex(0,0);

		// perform Early, Prompt and Late correlation
		/*!
		 * \todo Use SIMD-enabled correlators
		 */
		for(int i=0;i<d_current_prn_length_samples;i++) {
			//Perform the carrier wipe-off
			bb_signal_sample = in[i] * d_carr_sign[i];
			// Now get early, late, and prompt values for each
			d_Early += bb_signal_sample*d_early_code[i];
			d_Prompt += bb_signal_sample*d_prompt_code[i];
			d_Late += bb_signal_sample*d_late_code[i];
		}
		// Compute PLL error and update carrier NCO -
		carr_error=pll_cloop_two_quadrant_atan(d_Prompt)/ (float)TWO_PI;
		// Implement carrier loop filter and generate NCO command
		carr_nco=d_carrier_loop_filter.get_carrier_nco(carr_error);
		// Modify carrier freq based on NCO command
		d_carrier_doppler_hz = d_acq_carrier_doppler_hz + carr_nco;

		// Compute DLL error and update code NCO
		code_error=dll_nc_e_minus_l_normalized(d_Early,d_Late);
		// Implement code loop filter and generate NCO command
		code_nco=d_code_loop_filter.get_code_nco(code_error);
		// Modify code freq based on NCO command
		d_code_freq_hz = GPS_L1_CA_CODE_RATE_HZ - code_nco;

		// Update the phasestep based on code freq (variable) and
		// sampling frequency (fixed)
		d_code_phase_step_chips = d_code_freq_hz / (float)d_fs_in; //[chips]
		// variable code PRN sample block size
		float T_chip_seconds;
		float T_prn_seconds;
		float T_prn_samples;
		float K_blk_samples;
		T_chip_seconds=1/d_code_freq_hz;
		T_prn_seconds=T_chip_seconds*GPS_L1_CA_CODE_LENGTH_CHIPS;
		T_prn_samples=T_prn_seconds*d_fs_in;
		d_rem_code_phase_samples=d_next_rem_code_phase_samples;
		K_blk_samples=T_prn_samples+d_rem_code_phase_samples;
	    d_next_prn_length_samples=round(K_blk_samples);
	    d_next_rem_code_phase_samples=K_blk_samples-d_next_prn_length_samples;

		/*!
		 * \todo Improve the lock detection algorithm!
		 */
		// ####### CN0 ESTIMATION AND LOCK DETECTORS ######
		if (d_cn0_estimation_counter<CN0_ESTIMATION_SAMPLES)
		{
			// fill buffer with prompt correlator output values
			d_Prompt_buffer[d_cn0_estimation_counter]=d_Prompt;
			d_cn0_estimation_counter++;
		}else{
			d_cn0_estimation_counter=0;
			d_CN0_SNV_dB_Hz=gps_l1_ca_CN0_SNV(d_Prompt_buffer, CN0_ESTIMATION_SAMPLES,d_fs_in);
			d_carrier_lock_test=carrier_lock_detector(d_Prompt_buffer,CN0_ESTIMATION_SAMPLES);
			// ###### TRACKING UNLOCK NOTIFICATION #####

			int tracking_message;
	        if (d_carrier_lock_test<d_carrier_lock_threshold or d_carrier_lock_test>30)
	        {
	             d_carrier_lock_fail_counter++;
	        }else{
	        	if (d_carrier_lock_fail_counter>0) d_carrier_lock_fail_counter--;
	        }
	        if (d_carrier_lock_fail_counter>200)
	        {
	        	std::cout<<"Channel "<<d_channel << " loss of lock!\r\n";
	        	tracking_message=3; //loss of lock
	        	d_channel_internal_queue->push(tracking_message);
	        	d_carrier_lock_fail_counter=0;
	            d_current_prn_length_samples=(int)d_vector_length; //original dsp block length
	        	d_enable_tracking=false; // TODO: check if disabling tracking is consistent with the channel state machine

	        }
	        //std::cout<<"d_carrier_lock_fail_counter"<<d_carrier_lock_fail_counter<<"\r\n";
		}
		// Output the tracking data to navigation and PVT
		// Output channel 1: Prompt correlator output Q
		*out[0]=d_Prompt.real();
		// Output channel 2: Prompt correlator output I
		*out[1]=d_Prompt.imag();
		// Output channel 3: Current tracking time [ms]
		*out[2]=(float)(((double)d_sample_counter/(double)d_fs_in)*1000.0);
		// Output channel 4: Carrier accumulated phase
		*out[3]=d_acc_carrier_phase_rad;

		if(d_dump) {
			// MULTIPLEXED FILE RECORDING - Record results to file
			float prompt_I;
			float prompt_Q;
			float tmp_E,tmp_P,tmp_L;
			float tmp_float;
			prompt_I=d_Prompt.imag();
			prompt_Q=d_Prompt.real();
			tmp_E=std::abs<float>(d_Early);
			tmp_P=std::abs<float>(d_Prompt);
			tmp_L=std::abs<float>(d_Late);
	      	try {
				// EPR
				d_dump_file.write((char*)&tmp_E, sizeof(float));
				d_dump_file.write((char*)&tmp_P, sizeof(float));
				d_dump_file.write((char*)&tmp_L, sizeof(float));
				// PROMPT I and Q (to analyze navigation symbols)
				d_dump_file.write((char*)&prompt_I, sizeof(float));
				d_dump_file.write((char*)&prompt_Q, sizeof(float));
				// PRN start sample stamp
				tmp_float=(float)d_sample_counter;
				d_dump_file.write((char*)&tmp_float, sizeof(float));
				// accumulated carrier phase
				d_dump_file.write((char*)&d_acc_carrier_phase_rad, sizeof(float));

				// carrier and code frequency
				d_dump_file.write((char*)&d_carrier_doppler_hz, sizeof(float));
				d_dump_file.write((char*)&d_code_freq_hz, sizeof(float));

				//PLL commands
				d_dump_file.write((char*)&carr_error, sizeof(float));
				d_dump_file.write((char*)&carr_nco, sizeof(float));

				//DLL commands
				d_dump_file.write((char*)&code_error, sizeof(float));
				d_dump_file.write((char*)&code_nco, sizeof(float));

				// CN0 and carrier lock test
				d_dump_file.write((char*)&d_CN0_SNV_dB_Hz, sizeof(float));
				d_dump_file.write((char*)&d_carrier_lock_test, sizeof(float));

				// AUX vars (for debug purposes)
				tmp_float=0.0;
				d_dump_file.write((char*)&tmp_float, sizeof(float));
				tmp_float=0.0;
				d_dump_file.write((char*)&tmp_float, sizeof(float));
	      	 }
			  catch (std::ifstream::failure e) {
				std::cout << "Exception writing trk dump file "<<e.what()<<"\r\n";
			  }
		}
		// ########## DEBUG OUTPUT
		// debug: Second counter in channel 0
		if (d_channel==0)
		{
			if (floor(d_sample_counter/d_fs_in)!=d_last_seg)
			{
				d_last_seg=floor(d_sample_counter/d_fs_in);
				std::cout<<"t="<<d_last_seg<<std::endl;
				std::cout<<"TRK CH "<<d_channel<<" CN0="<<d_CN0_SNV_dB_Hz<< std::endl;
				std::cout<<"TRK CH "<<d_channel<<" Carrier_lock_test="<<d_carrier_lock_test<< std::endl;
			}
		}else
		{
			if (floor(d_sample_counter/d_fs_in)!=d_last_seg)
			{
				d_last_seg=floor(d_sample_counter/d_fs_in);
				std::cout<<"TRK CH "<<d_channel<<" CN0="<<d_CN0_SNV_dB_Hz<< std::endl;
				std::cout<<"TRK CH "<<d_channel<<" Carrier_lock_test="<<d_carrier_lock_test<< std::endl;
			}
		}
	}
	consume_each(d_current_prn_length_samples); // this is necesary in gr_block derivates
        d_sample_counter+=d_current_prn_length_samples; //count for the processed samples
	return 1; //output tracking result ALWAYS even in the case of d_enable_tracking==false
}

void gps_l1_ca_dll_pll_tracking_cc::set_acq_code_phase(float code_phase) {
	d_acq_code_phase_samples = code_phase;
	LOG_AT_LEVEL(INFO) << "Tracking code phase set to " << d_acq_code_phase_samples;
}

void gps_l1_ca_dll_pll_tracking_cc::set_acq_doppler(float doppler) {
	d_acq_carrier_doppler_hz = doppler;
	LOG_AT_LEVEL(INFO) << "Tracking carrier doppler set to " << d_acq_carrier_doppler_hz;
}

void gps_l1_ca_dll_pll_tracking_cc::set_satellite(unsigned int satellite) {
	d_satellite = satellite;
	LOG_AT_LEVEL(INFO) << "Tracking Satellite set to " << d_satellite;
}

void gps_l1_ca_dll_pll_tracking_cc::set_channel(unsigned int channel) {
	d_channel = channel;
	LOG_AT_LEVEL(INFO) << "Tracking Channel set to " << d_channel;
}

void gps_l1_ca_dll_pll_tracking_cc::set_acq_sample_stamp(unsigned long int sample_stamp)
{
    d_acq_sample_stamp = sample_stamp;
}

void gps_l1_ca_dll_pll_tracking_cc::set_channel_queue(concurrent_queue<int> *channel_internal_queue)
{
    d_channel_internal_queue = channel_internal_queue;
}
