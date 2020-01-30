/*
 * Copyright 2017-2018 Leo McCormack
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Filename: ambi_dec_internal.c
 * -----------------------------
 * A frequency-dependent Ambisonic decoder for loudspeakers or headphones.
 * Different decoder settings can be specified for the low and high frequencies.
 * When utilising spherical harmonic signals derived from real microphone
 * arrays, this implementation also allows the decoding order per frequency band
 * to be specified. Optionally, a SOFA file may be loaded for personalised
 * headphone listening.
 * The algorithms utilised in this Ambisonic decoder were pieced together and
 * developed in collaboration with Archontis Politis.
 *
 * Dependencies:
 *     saf_utilities, afSTFTlib, saf_hoa, saf_vbap, saf_hrir, saf_sh,
 *     saf_sofa_reader
 * Author, date created:
 *     Leo McCormack, 07.12.2017
 */

#include "ambi_dec_internal.h" 

void ambi_dec_setCodecStatus(void* const hAmbi, CODEC_STATUS newStatus)
{
    ambi_dec_data *pData = (ambi_dec_data*)(hAmbi);
    if(newStatus==CODEC_STATUS_NOT_INITIALISED){
        /* Pause until current initialisation is complete */
        while(pData->codecStatus == CODEC_STATUS_INITIALISING)
            SAF_SLEEP(10);
    }
    pData->codecStatus = newStatus;
}

void ambi_dec_interpHRTFs
(
    void* const hAmbi,
    float azimuth_deg,
    float elevation_deg,
    float_complex h_intrp[HYBRID_BANDS][NUM_EARS]
)
{
    ambi_dec_data *pData = (ambi_dec_data*)(hAmbi);
    codecPars* pars = pData->pars;
    int i, band;
    int aziIndex, elevIndex, N_azi, idx3d;
    float_complex ipd;
    float aziRes, elevRes, weights[1][3], itds3[3],  itdInterp[1];
    float magnitudes3[HYBRID_BANDS][3][NUM_EARS], magInterp[HYBRID_BANDS][NUM_EARS];

    /* find closest pre-computed VBAP direction */
    aziRes = (float)pars->hrtf_vbapTableRes[0];
    elevRes = (float)pars->hrtf_vbapTableRes[1];
    N_azi = (int)(360.0f / aziRes + 0.5f) + 1;
    aziIndex = (int)(matlab_fmodf(azimuth_deg + 180.0f, 360.0f) / aziRes + 0.5f);
    elevIndex = (int)((elevation_deg + 90.0f) / elevRes + 0.5f);
    idx3d = elevIndex * N_azi + aziIndex;
    for (i = 0; i < 3; i++)
        weights[0][i] = pars->hrtf_vbap_gtableComp[idx3d*3 + i];
    
    /* retrieve the 3 itds and hrtf magnitudes */
    for (i = 0; i < 3; i++) {
        itds3[i] = pars->itds_s[pars->hrtf_vbap_gtableIdx[idx3d*3+i]];
        for (band = 0; band < HYBRID_BANDS; band++) {
            magnitudes3[band][i][0] = pars->hrtf_fb_mag[band*NUM_EARS*(pars->N_hrir_dirs) + 0*(pars->N_hrir_dirs) + pars->hrtf_vbap_gtableIdx[idx3d*3+i]];
            magnitudes3[band][i][1] = pars->hrtf_fb_mag[band*NUM_EARS*(pars->N_hrir_dirs) + 1*(pars->N_hrir_dirs) + pars->hrtf_vbap_gtableIdx[idx3d*3+i]];
        }
    }
    
    /* interpolate hrtf magnitudes and itd seperately */
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 1, 1, 3, 1,
                (float*)weights, 3,
                (float*)itds3, 1, 0,
                (float*)itdInterp, 1);
    for (band = 0; band < HYBRID_BANDS; band++) {
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 1, 2, 3, 1,
                    (float*)weights, 3,
                    (float*)magnitudes3[band], 2, 0,
                    (float*)magInterp[band], 2);
    }
    
    /* reintroduce the interaural phase difference per band */
    for (band = 0; band < HYBRID_BANDS; band++) {
        if(pData->freqVector[band]<1.5e3f)
            ipd = cmplxf(0.0f, (matlab_fmodf(2.0f*PI* (pData->freqVector[band]) * itdInterp[0] + PI, 2.0f*PI) - PI) / 2.0f);
        else
            ipd = cmplxf(0.0f, (matlab_fmodf(2.0f*PI* (pData->freqVector[band]) * itdInterp[0] + PI, 2.0f*PI) - PI) / 6.0f);
        h_intrp[band][0] = ccmulf(cmplxf(magInterp[band][0], 0.0f), cexpf(ipd));
        h_intrp[band][1] = ccmulf(cmplxf(magInterp[band][1], 0.0f), conjf(cexpf(ipd)));
    }
}

void loadLoudspeakerArrayPreset
(
    LOUDSPEAKER_ARRAY_PRESETS preset,
    float dirs_deg[MAX_NUM_LOUDSPEAKERS_IN_PRESET][2],
    int* newNCH,
    int* nDims
)
{
    float sum_elev;
    int ch, i, nCH;
    
    switch(preset){
        default:
        case LOUDSPEAKER_ARRAY_PRESET_DEFAULT:
        case LOUDSPEAKER_ARRAY_PRESET_5PX:
            nCH = 5;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __5pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_7PX:
            nCH = 7;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __7pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_8PX:
            nCH = 8;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __8pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_9PX:
            nCH = 9;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __9pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_10PX:
            nCH = 10;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __10pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_11PX:
            nCH = 11;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __11pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_11PX_7_4:
            nCH = 11;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __11pX_7_4_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_13PX:
            nCH = 13;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __13pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_22PX:
            nCH = 22;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __22pX_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_AALTO_MCC:
            nCH = 44;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Aalto_MCC_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_AALTO_APAJA:
            nCH = 29;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Aalto_Apaja_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_AALTO_LR:
            nCH = 13;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Aalto_LR_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_DTU_AVIL:
            nCH = 64;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __DTU_AVIL_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_ZYLIA_LAB:
            nCH = 22;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Zylia_Lab_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_4:
            nCH = 4;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_2_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_12:
            nCH = 12;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_4_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_24:
            nCH = 24;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_6_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_36:
            nCH = 36;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_8_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_48:
            nCH = 48;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_9_dirs_deg[ch][i];
            break;
        case LOUDSPEAKER_ARRAY_PRESET_T_DESIGN_60:
            nCH = 60;
            for(ch=0; ch<nCH; ch++)
                for(i=0; i<2; i++)
                    dirs_deg[ch][i] = __Tdesign_degree_10_dirs_deg[ch][i];
            break;
    }
    
    /* Fill remaining slots with default coords */
    for(; ch<MAX_NUM_LOUDSPEAKERS_IN_PRESET; ch++)
        for(i=0; i<2; i++)
            dirs_deg[ch][i] = default_LScoords64_rad[ch][i]* (180.0f/M_PI);
    
    /* specify new number of channels (for dynamically changing the number of TFT channels) */
    (*newNCH) = nCH;
    
    /* Estimate number of dimensions.
     * (Fails if using 2D setups are not on the horizontal plane ) */
    sum_elev = 0.0f;
    for(i=0; i<nCH; i++)
        sum_elev += fabsf(dirs_deg[i][1]);
    if(sum_elev < 0.01f)
        (*nDims) = 2;
    else
        (*nDims) = 3;
}
