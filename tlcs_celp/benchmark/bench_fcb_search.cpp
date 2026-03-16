#include <stdio.h>
#include <stdlib.h>
#include "../codec/celp.h"
#ifdef _WIN32
#include <windows.h>
#else
#error "Only windows supported for now. Need seperate timing routine implemented"
#endif

int main( int argc, char** argv )
{
    printf("Benchmarking of fcb search !!\n");
    if(argc != 2){
        printf("Error missing input file\n");
    }

    float wnrg, gain_from_search, fcb_wnrg;

    LARGE_INTEGER frequency;        // ticks per second
    LARGE_INTEGER t1, t2;           // ticks
    QueryPerformanceFrequency(&frequency);

    FILE *result_file = fopen("timing_results.txt","wt");    
    for(int16_t num_surv_override = 2; num_surv_override <= 6; num_surv_override+=2){
        for(int16_t fcb_pulses_max_override = 1; fcb_pulses_max_override <= 10; fcb_pulses_max_override++){
            FILE* fp = NULL;
            fopen_s(&fp, argv[1],"rb");
            if(fp == NULL){
                printf("File not found %s\n", argv[1]);
                return -1;
            }
            int16_t sf_len, num_surv;
            size_t n = fread(&sf_len, sizeof(int16_t), 1, fp);
            n = fread(&num_surv, sizeof(int16_t), 1, fp);
            uint64_t *sgntr = (uint64_t *)malloc(sizeof(uint64_t)*sf_len);
            n = fread(sgntr, sizeof(uint64_t), sf_len, fp);  
            float *Phi = (float *)malloc(sizeof(uint64_t)*sf_len);
            float *d = (float *)malloc(sizeof(uint64_t)*sf_len);
            int16_t *pulses = (int16_t *)malloc(sizeof(int16_t)*sf_len);   

            num_surv = num_surv_override > 0 ? num_surv_override : num_surv;

            void *st = create_fcb_search(sgntr, sf_len, num_surv);

            double elapsedTime, totElapsedTime = 0.0, totElapsedTimeToplz = 0.0, totElapsedTimeNonToplz = 0.0;
            int nFrames = 0, n_toplz = 0;
            while(n > 0){
                int16_t fcb_pulses_max, lag;
                n = fread(&fcb_pulses_max, sizeof(int16_t), 1, fp);
                if(n == 0){
                    break;
                }
                n = fread(&lag, sizeof(int16_t), 1, fp);
                float wnrg_per_pulse, pitch_sharp;
                n = fread(&wnrg_per_pulse, sizeof(float), 1, fp);
                n = fread(&pitch_sharp, sizeof(float), 1, fp);
                n = fread(Phi, sizeof(float), sf_len, fp);
                n = fread(d, sizeof(float), sf_len, fp);
                nFrames++;

                fcb_pulses_max = fcb_pulses_max_override > 0 ? fcb_pulses_max_override : fcb_pulses_max;

                QueryPerformanceCounter(&t1);
                int ret = fcb_search_deldec( st, Phi, d, pulses, &wnrg, &gain_from_search, &fcb_wnrg, wnrg_per_pulse, pitch_sharp, lag, fcb_pulses_max);  
                QueryPerformanceCounter(&t2);      
                elapsedTime = (t2.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
                totElapsedTime += elapsedTime;
                if(lag < sf_len && pitch_sharp != 0.0f){
                    totElapsedTimeNonToplz += elapsedTime;
                }else{
                    totElapsedTimeToplz += elapsedTime;
                    n_toplz++;
                }
                int16_t n_pulses;
                n = fread(&n_pulses, sizeof(int16_t), 1, fp);
                int16_t target_pulses[100];
                if(n_pulses > 0){
                    n = fread(target_pulses, sizeof(int16_t), n_pulses, fp);
                }
                if(fcb_pulses_max_override == 0){
                    int err = 0;
                    if(n_pulses != ret){
                        err = 1;
                    }else{
                        for(int i = 0; i < n_pulses; i++){
                            if(pulses[i] != target_pulses[i]){
                                err = 1;
                            }
                        }
                    }
                    if(err){
                        break;
                    }
                }
            }

            fprintf(result_file,"%d %d %d %f %f %f \n", sf_len, fcb_pulses_max_override, num_surv_override, 
                100.0*((totElapsedTime/nFrames) * 16) / sf_len, 100.0*((totElapsedTimeToplz/n_toplz) * 16) / sf_len, 100.0*((totElapsedTimeNonToplz/(nFrames-n_toplz)) * 16) / sf_len);

            fclose(fp);
            delete_fcb_search(st);
            free(Phi);
            free(d);
            free(pulses);  
        }
    }
    fclose(result_file);
}