#include <stdio.h>
#include <librosa.h>
#include <vector>
#include <limits>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <ctime>
#include <memory>
#include <axcl.h>

#include "cmdline.h"
#include "AudioFile.h"
#include "utilities/base64.h"
#include "opencc/opencc.h"
#include "middleware/axcl_runtime_runner.hpp"
#include "middleware/axcl_native_runner.hpp"
#include "utilities/timer.hpp"

#define WHISPER_SAMPLE_RATE 16000
#define WHISPER_N_FFT       400
#define WHISPER_HOP_LENGTH  160
#define WHISPER_CHUNK_SIZE  30
#define WHISPER_N_MELS      80

#define WHISPER_SOT         50258
#define WHISPER_EOT         50257
#define WHISPER_BLANK       220
#define WHISPER_NO_TIMESTAMPS   50363
#define WHISPER_NO_SPEECH   50362
#define WHISPER_TRANSLATE   50358
#define WHISPER_TRANSCRIBE  50359
#define WHISPER_VOCAB_SIZE  51865
#define WHISPER_N_TEXT_CTX  448
#define NEG_INF             -std::numeric_limits<float>::infinity()

const char* CONFIG_FILE_DEFAULT = "/usr/local/axcl/axcl.json";

static std::vector<int> WHISPER_LANG_CODES{
    50273,50303,50288,50261,50342,50299,50330,50302,50336,50267,50287,50292,50294,50323,50348,50291,50317,
    50326,50289,50356,50290,50282,50347,50331,50354,50264,50333,50296,50339,50318,50305,50293,50280,50322,
    50312,50306,50353,50285,50275,50340,50278,50268,50337,50316,50266,50307,50310,50338,50334,50313,50351,
    50260,50344,50283,50327,50272,50324,50276,50281,50301,50332,50300,50309,50343,50349,50335,50320,50259,
    50284,50304,50277,50311,50319,50314,50352,50328,50286,50274,50329,50270,50269,50350,50263,50345,50298,
    50279,50297,50262,50315,50321,50308,50355,50265,50346,50295,50271,50357,50341,50325
};

static std::vector<std::string> WHISPER_LANG_NAMES{
    "sv","sr","no","de","nn","te", "be","bn","lo","pt","ta","bg","la","km","tl","hr","sq","so","th","jw","ur","ms","bo",
    "tg","ha","ko","gu","ml","ht", "sw","sl","lt","uk","si","hy","kn","ln","da","id","ps","vi","tr","uz","kk","ja","et",
    "eu","fo","am","ne","tt","zh", "sa","cs","af","ar","sn","hi","el","lv","sd","fa","br","mt","mg","yi","mr","en","ro",
    "az","fi","is","gl","mn","haw","oc","hu","it","ka","ca","pl","as","ru","lb","sk","he","cy","es","bs","pa","mk","ba",
    "fr","my","mi","nl","su","tk", "yo"
};

static std::unordered_map<std::string, int> WHISPER_N_TEXT_STATE_MAP{
    {"tiny",    384},
    {"small",   768}
};

static std::vector<int32_t> SOT_SEQUENCE{WHISPER_SOT,50260,WHISPER_TRANSCRIBE,WHISPER_NO_TIMESTAMPS};


static void supress_tokens(std::vector<float>& logits, bool is_initial) {
    if (is_initial) {
        logits[WHISPER_EOT] = NEG_INF;
        logits[WHISPER_BLANK] = NEG_INF;
    }

    logits[WHISPER_NO_TIMESTAMPS] = NEG_INF;
    logits[WHISPER_SOT] = NEG_INF;
    logits[WHISPER_NO_SPEECH] = NEG_INF;
    logits[WHISPER_TRANSLATE] = NEG_INF;
}

static int argmax(const std::vector<float>& logits) {
    auto max_iter = std::max_element(logits.begin(), logits.end());
    return std::distance(logits.begin(), max_iter); // absolute index of max
}

static int detect_language(const std::string& language) {
    int i = 51; // zh
    for (int n = 0; n < WHISPER_LANG_CODES.size(); n++) {
        if (language == WHISPER_LANG_NAMES[n]) {
            i = n;
            break;
        }
    }
    
    return WHISPER_LANG_CODES[i];
}

std::unique_ptr<middleware::runner> load_runner(const std::string& model_path) {
    std::unique_ptr<middleware::runner> runner = std::make_unique<middleware::runtime_runner>();

    if (!runner->init(CONFIG_FILE_DEFAULT, 0, 0)) {
        fprintf(stderr, "[ERROR] Init failed.\n");
        return nullptr;
    }

    if (!runner->load(model_path)) {
        fprintf(stderr, "[ERROR] Loading model {%s} failed.\n", model_path.c_str());
        return nullptr;
    }

    if (!runner->prepare(true, true, 0, 0)) {
        fprintf(stderr, "[ERROR] Prepare for model {%s} failed.\n", model_path.c_str());
        return nullptr;
    }

    return std::move(runner);
}


int main(int argc, char** argv) {
    cmdline::parser cmd;
    cmd.add<std::string>("encoder", 'e', "encoder axmodel", false, "../models/small-encoder.axmodel");
    cmd.add<std::string>("decoder_main", 'm', "decoder_main axmodel", false, "../models/small-decoder-main.axmodel");
    cmd.add<std::string>("decoder_loop", 'l', "decoder_loop axmodel", false, "../models/small-decoder-loop.axmodel");
    cmd.add<std::string>("position_embedding", 'p', "position_embedding.bin", false, "../models/small-positional_embedding.bin");
    cmd.add<std::string>("token", 't', "tokens txt", false, "../models/small-tokens.txt");
    cmd.add<std::string>("wav", 'w', "wav file", true, "");
    cmd.add<std::string>("model_type", 0, "tiny, small, large", false, "small");
    cmd.add<std::string>("language", 0, "en, zh", false, "zh");
    cmd.parse_check(argc, argv);

    // 0. get app args, can be removed from user's app
    auto encoder_file = cmd.get<std::string>("encoder");
    auto decoder_main_file = cmd.get<std::string>("decoder_main");
    auto decoder_loop_file = cmd.get<std::string>("decoder_loop");
    auto pe_file = cmd.get<std::string>("position_embedding");
    auto token_file = cmd.get<std::string>("token");
    auto wav_file = cmd.get<std::string>("wav");
    auto model_type = cmd.get<std::string>("model_type");
    auto language = cmd.get<std::string>("language");

    if (WHISPER_N_TEXT_STATE_MAP.find(model_type) == WHISPER_N_TEXT_STATE_MAP.end()) {
        fprintf(stderr, "Can NOT find n_text_state for model_type: %s\n", model_type.c_str());
        return -1;
    }

    int WHISPER_N_TEXT_STATE = WHISPER_N_TEXT_STATE_MAP[model_type];
    clock_t start, end;

    printf("encoder: %s\n", encoder_file.c_str());
    printf("decoder_main: %s\n", decoder_main_file.c_str());
    printf("decoder_loop: %s\n", decoder_loop_file.c_str());
    printf("wav_file: %s\n", wav_file.c_str());
    printf("language: %s\n", language.c_str());

    utilities::timer timer;
    timer.start();
    auto encoder = load_runner(encoder_file);
    if (!encoder) {
        printf("Load encoder failed!\n");
        return -1;
    }
    timer.stop();
    printf("Load encoder take %.2f ms\n", timer.elapsed<utilities::timer::milliseconds>());

    timer.start();
    auto decoder_main = load_runner(decoder_main_file);
    if (!decoder_main) {
        printf("Load decoder_main failed!\n");
        return -1;
    }
    timer.stop();
    printf("Load decoder_main take %.2f ms\n", timer.elapsed<utilities::timer::milliseconds>());

    timer.start();
    auto decoder_loop = load_runner(decoder_loop_file);
    if (!decoder_loop) {
        printf("Load decoder_loop failed!\n");
        return -1;
    }
    timer.stop();
    printf("Load decoder_loop take %.2f ms\n", timer.elapsed<utilities::timer::milliseconds>());

    AudioFile<float> audio_file;
    if (!audio_file.load(wav_file)) {
        printf("load wav failed!\n");
        return -1;
    }

    auto& samples = audio_file.samples[0];
    int n_samples = samples.size();

    printf("Read positional_embedding\n");
    std::vector<float> positional_embedding(WHISPER_N_TEXT_CTX * WHISPER_N_TEXT_STATE);
    FILE* fp = fopen(pe_file.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "Can NOT open %s\n", pe_file.c_str());
        return -1;
    }
    fread(positional_embedding.data(), sizeof(float), WHISPER_N_TEXT_CTX * WHISPER_N_TEXT_STATE, fp);
    fclose(fp);

    std::vector<std::string> token_tables;
    std::ifstream ifs(token_file);
    if (!ifs.is_open()) {
        fprintf(stderr, "Can NOT open %s\n", token_file.c_str());
        return -1;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        size_t i = line.find(' ');
        token_tables.push_back(line.substr(0, i));
    }

    auto mel = librosa::Feature::melspectrogram(samples, WHISPER_SAMPLE_RATE, WHISPER_N_FFT, WHISPER_HOP_LENGTH, "hann", true, "reflect", 2.0f, WHISPER_N_MELS, 0.0f, WHISPER_SAMPLE_RATE / 2.0f);
    int n_mel = mel.size();
    int n_len = mel[0].size();

    // clamping and normalization
    double mmax = -1e20;
    for (int i = 0; i < WHISPER_N_MELS; i++) {
        for (int n = 0; n < n_len; n++) {
            mel[i][n] = std::log10(std::max(mel[i][n], 1e-10f));

            if (mel[i][n] > mmax) {
                mmax = mel[i][n] ;
            }
        }
    }

    for (int i = 0; i < WHISPER_N_MELS; i++) {
        for (int n = 0; n < n_len; n++) {
            mel[i][n] = (std::max(mel[i][n], (float)(mmax - 8.0)) + 4.0)/4.0;
            mel[i].resize(3000);
        }
    }

    n_len = mel[0].size();

    int offset = 0;
    std::vector<float> logits(WHISPER_VOCAB_SIZE);
    int max_token_id = -1;
    std::vector<int> results;
    std::vector<int> tokens(1);
    bool is_broke = false;
    
    std::vector<float> n_layer_cross_k(encoder->get_output_size(0) / sizeof(float));
    std::vector<float> n_layer_cross_v(encoder->get_output_size(1) / sizeof(float));

    std::vector<float> decoder_main_logits(4 * WHISPER_VOCAB_SIZE);
    std::vector<float> n_layer_self_k_cache(decoder_main->get_output_size(1) / sizeof(float));
    std::vector<float> n_layer_self_v_cache(decoder_main->get_output_size(2) / sizeof(float));

    // encoder
    std::vector<float> continous_mel(WHISPER_N_MELS * n_len);
    for (int i = 0; i < n_mel; i++) {
        memcpy(continous_mel.data() + i * n_len, mel[i].data(), sizeof(float) * n_len);
    }

    // fp = fopen("mel.bin", "wb");
    // fwrite(continous_mel.data(), sizeof(float), continous_mel.size(), fp);
    // fclose(fp);

    // fp = fopen("../../whisper.axera/cpp/mel.bin", "rb");
    // fread(continous_mel.data(), sizeof(float), continous_mel.size(), fp);
    // fclose(fp);

    axclrtMemcpy(encoder->get_input_pointer(0), continous_mel.data(), sizeof(float) * continous_mel.size(), AXCL_MEMCPY_HOST_TO_DEVICE);
    if (!encoder->run(false)) {
        printf("encoder run failed!\n");
        return -1;
    }
    // axclrtMemcpy(n_layer_cross_k.data(), encoder->get_output_pointer(0), sizeof(float) * n_layer_cross_k.size(), AXCL_MEMCPY_DEVICE_TO_HOST);
    // axclrtMemcpy(n_layer_cross_v.data(), encoder->get_output_pointer(1), sizeof(float) * n_layer_cross_v.size(), AXCL_MEMCPY_DEVICE_TO_HOST);

    // fp = fopen("n_layer_cross_k.bin", "wb");
    // fwrite(n_layer_cross_k.data(), sizeof(float), n_layer_cross_k.size(), fp);
    // fclose(fp);

    // fp = fopen("n_layer_cross_v.bin", "wb");
    // fwrite(n_layer_cross_v.data(), sizeof(float), n_layer_cross_v.size(), fp);
    // fclose(fp);

    // detect language
    SOT_SEQUENCE[1] = detect_language(language);

    // decoder_main
    timer.start();
    axclrtMemcpy(decoder_main->get_input_pointer(0), SOT_SEQUENCE.data(), sizeof(int) * SOT_SEQUENCE.size(), AXCL_MEMCPY_HOST_TO_DEVICE);
    axclrtMemcpy(decoder_main->get_input_pointer(1), encoder->get_output_pointer(0), decoder_main->get_input_size(1), AXCL_MEMCPY_DEVICE_TO_DEVICE);
    axclrtMemcpy(decoder_main->get_input_pointer(2), encoder->get_output_pointer(1), decoder_main->get_input_size(2), AXCL_MEMCPY_DEVICE_TO_DEVICE);
    if (!decoder_main->run(false)) {
        printf("decoder_main run failed!\n");
        return -1;
    }
    axclrtMemcpy(decoder_main_logits.data(), decoder_main->get_output_pointer(0), sizeof(float) * decoder_main_logits.size(), AXCL_MEMCPY_DEVICE_TO_HOST);
    // axclrtMemcpy(n_layer_self_k_cache.data(), decoder_main->get_output_pointer(1), sizeof(float) * n_layer_self_k_cache.size(), AXCL_MEMCPY_DEVICE_TO_HOST);
    // axclrtMemcpy(n_layer_self_v_cache.data(), decoder_main->get_output_pointer(2), sizeof(float) * n_layer_self_v_cache.size(), AXCL_MEMCPY_DEVICE_TO_HOST);
    timer.stop();

    offset += SOT_SEQUENCE.size();
    // logits = logits[0, -1]
    std::copy(decoder_main_logits.begin() + 3 * WHISPER_VOCAB_SIZE, decoder_main_logits.end(), logits.begin());
    supress_tokens(logits, true);
    max_token_id = argmax(logits);

    // fp = fopen("logits.bin", "wb");
    // fwrite(logits.data(), sizeof(float),logits.size(), fp);
    // fclose(fp);

    float first_token_cost = timer.elapsed<utilities::timer::milliseconds>();
    printf("First token: %d \t take %.2fms\n", max_token_id, first_token_cost);

    std::vector<float> mask(WHISPER_N_TEXT_CTX);
    for (int n = 0; n < WHISPER_N_TEXT_CTX - offset - 1; n++) {
        mask[n] = NEG_INF;
    }

    // fp = fopen("logits.bin", "wb");
    // fwrite(logits.data(), sizeof(float), logits.size(), fp);
    // fclose(fp);
    axclrtMemcpy(decoder_loop->get_input_pointer(1), decoder_main->get_output_pointer(1), decoder_loop->get_input_size(1), AXCL_MEMCPY_DEVICE_TO_DEVICE);
    axclrtMemcpy(decoder_loop->get_input_pointer(2), decoder_main->get_output_pointer(2), decoder_loop->get_input_size(2), AXCL_MEMCPY_DEVICE_TO_DEVICE);

    // for (int i = 0; i < decoder_loop->get_input_count(); i++) {
    //     printf("decoder_loop input[%d] name: %s size: %d\n", i, decoder_loop->get_input_name(i).c_str(), decoder_loop->get_input_size(i));
    // }
    // for (int i = 0; i < decoder_loop->get_output_count(); i++) {
    //     printf("decoder_loop output[%d] name: %s size: %d\n", i, decoder_loop->get_output_name(i).c_str(), decoder_loop->get_output_size(i));
    // }
    axclrtMemcpy(decoder_loop->get_input_pointer(3), encoder->get_output_pointer(0), decoder_loop->get_input_size(3), AXCL_MEMCPY_DEVICE_TO_DEVICE);
    axclrtMemcpy(decoder_loop->get_input_pointer(4), encoder->get_output_pointer(1), decoder_loop->get_input_size(4), AXCL_MEMCPY_DEVICE_TO_DEVICE);

    utilities::timer loop_timer;
    loop_timer.start();
    int loop_token_num = 0;
    for (int i = 0; i < WHISPER_N_TEXT_CTX - SOT_SEQUENCE.size(); i++) {
        if (max_token_id == WHISPER_EOT) {
            is_broke = true;
            break;
        }

        utilities::timer token_timer;
        token_timer.start();

        results.push_back(max_token_id);
        tokens[0] = results.back();
        // mask[:model.n_text_ctx - offset[0] - 1] = -torch.inf
       
        // inference
        // timer.start();
        axclrtMemcpy(decoder_loop->get_input_pointer(0), tokens.data(), sizeof(int) * tokens.size(), AXCL_MEMCPY_HOST_TO_DEVICE);
        axclrtMemcpy(decoder_loop->get_input_pointer(5), positional_embedding.data() + offset * WHISPER_N_TEXT_STATE, decoder_loop->get_input_size(5), AXCL_MEMCPY_HOST_TO_DEVICE);
        axclrtMemcpy(decoder_loop->get_input_pointer(6), mask.data(), sizeof(float) * mask.size(), AXCL_MEMCPY_HOST_TO_DEVICE);
        // timer.stop();
        // printf("Memcpy input take %.2fms\n", timer.elapsed<utilities::timer::milliseconds>());

        // timer.start();
        if (!decoder_loop->run(false)) {
            printf("decoder_loop run failed!\n");
            return -1;
        } 
        // timer.stop();
        // printf("Run take %.2fms\n", timer.elapsed<utilities::timer::milliseconds>());

        // timer.start();
        axclrtMemcpy(decoder_loop->get_input_pointer(1), decoder_loop->get_output_pointer(1), sizeof(float) * n_layer_self_k_cache.size(), AXCL_MEMCPY_DEVICE_TO_DEVICE);
        axclrtMemcpy(decoder_loop->get_input_pointer(2), decoder_loop->get_output_pointer(2), sizeof(float) * n_layer_self_v_cache.size(), AXCL_MEMCPY_DEVICE_TO_DEVICE);
        // timer.stop();
        // printf("Memcpy KVCache (DEVICE TO DEVICE) take %.2fms\n", timer.elapsed<utilities::timer::milliseconds>());

        // get logits output
        // timer.start();
        axclrtMemcpy(logits.data(), decoder_loop->get_output_pointer(0), sizeof(float) * logits.size(), AXCL_MEMCPY_DEVICE_TO_HOST);
        // timer.stop();
        // printf("Memcpy output logits take %.2fms\n", timer.elapsed<utilities::timer::milliseconds>());

        offset += 1;
        mask[WHISPER_N_TEXT_CTX - offset - 1] = 0;
 
        supress_tokens(logits, false);
        max_token_id = argmax(logits);  

        loop_token_num++;

        token_timer.stop();
        printf("Next Token: %d \t take %.2f ms\n", max_token_id, token_timer.elapsed<utilities::timer::milliseconds>());
    }
    loop_timer.stop();
    float loop_cost = loop_timer.elapsed<utilities::timer::milliseconds>() + first_token_cost;
    printf("All Token: take %.2fms, %.2f token/s \n", loop_cost, (loop_token_num + 1) * 1000.0f / loop_cost);

    std::string s;
    for (const auto i : results) {
        char str[1024];
        base64_decode((const uint8*)token_tables[i].c_str(), (uint32)token_tables[i].size(), str);
        s += str;
    }

    if (language == "en") {
        printf("Result: %s\n", s.c_str());
    }
    else {
        const opencc::SimpleConverter converter("t2s.json");
        std::string simple_str = converter.Convert(s);
        printf("Result: %s\n", simple_str.c_str());
    }

    return 0;
}
    