#include "Fonduempeg.h"
#include <nlohmann/json.hpp>
#include<fstream>

#define PATH_TO_CONFIG_FILE "/home/tb1516/cppdev/fondue/config_files/config.json"

using json = nlohmann::json;

std::mutex new_source_mtx;


void continue_streaming (InputStream& source, OutputStream& sink, 
                         std::chrono::_V2::steady_clock::time_point& end_time, ControlFlags& flags);
InputStream&& crossfade (InputStream& source, InputStream& new_source, 
                        OutputStream& sink, std::chrono::_V2::steady_clock::time_point& end_time, ControlFlags& flags);
void audio_processing (InputStream &source, InputStream &new_source, 
                        OutputStream &sink, ControlFlags& flags);
void audio_processing (InputStream &source, InputStream &new_source, 
                        OutputStream &sink, ControlFlags& flags);
void control (InputStream &new_source, const AVCodecContext& 
                        output_codec_ctx, ControlFlags& flags);
bool find_and_remove(std::string& command, const std::string& substring);

int main ()
{
    std::ifstream f{PATH_TO_CONFIG_FILE};
    json config = json::parse(f);
    FFMPEGString input_prompt{config["test input"]};
    FFMPEGString output_prompt{config["test output"]};
    f.close();

    avdevice_register_all();
    OutputStream sink{output_prompt};
    InputStream source {};
    InputStream new_source{};
    ControlFlags flags{};
    
    try
    {    
        source = InputStream {input_prompt, sink.get_output_codec_context(),
                                        SourceTimingModes::realtime, DefaultSourceModes::white_noise};
    }
    catch (const char* exception)
    {
        std::cout<<exception<<": failed to correctly access input, switching to default source\n";
        source = InputStream {sink.get_output_codec_context(), DefaultSourceModes::white_noise};
    }
    
    std::thread audioThread(audio_processing, std::ref(source), std::ref(new_source), std::ref(sink), std::ref(flags));
    std::thread controlThread(control, std::ref(new_source), std::ref(sink.get_output_codec_context()), std::ref(flags));

    audioThread.join();
    controlThread.join();

    return 0;
}



void audio_processing (InputStream &source, InputStream &new_source, 
                        OutputStream &sink, ControlFlags& flags)
{
    std::chrono::_V2::steady_clock::time_point end_time = std::chrono::steady_clock::now();

    while (!flags.stop)
    {
        if (flags.normal_streaming)
        {
            continue_streaming(source, sink, end_time, flags);
            continue;
        }            
        else 
        {  
            std::lock_guard<std::mutex> lock (new_source_mtx);
            source = crossfade(source, new_source, sink, end_time, flags);
            flags.normal_streaming = true;
        }    
    }    
    sink.finish_streaming();
}

void control(InputStream &new_source, const AVCodecContext& output_codec_ctx, ControlFlags& flags)
{
    std::chrono::duration<int> refresh_interval(1);
    int count {};
    SourceTimingModes timing_mode = SourceTimingModes::realtime; 
    DefaultSourceModes source_mode = DefaultSourceModes::white_noise;
    std::string command {};

    while (!flags.stop)
    {
        // if (count == 60)
        // {
        //     std::ifstream f {PATH_TO_CONFIG_FILE};
        //     json config = json::parse(f);
        //     FFMPEGString new_input {config["test input 2 dramac"]};

        //     std::lock_guard<std::mutex> lock (new_source_mtx);
        //     new_source = InputStream{new_input, output_codec_ctx, timing_mode, source_mode};
        //     flags.normal_streaming = false;  
        // }

        // if (count == 120)
        // {
        //     std::ifstream f {PATH_TO_CONFIG_FILE};
        //     json config = json::parse(f);
        //     FFMPEGString new_input {config["test input dramac"]};

        //     std::lock_guard<std::mutex> lock (new_source_mtx);
        //     new_source = InputStream{new_input, output_codec_ctx, timing_mode, source_mode};
        //     flags.normal_streaming = false;  
        //     count = 0;
        // }
        // count ++;
        // std::this_thread::sleep_for(refresh_interval);
        std::getline(std::cin, command);
        //kill
        if (command == "kill")
        {
            flags.stop = true;
        }
        //list-sources
        if (command == "list-sources")
        {
            std::ifstream f {PATH_TO_CONFIG_FILE};
            json config = json::parse(f);
            for (auto it = config.begin(); it != config.end(); ++it)
            {
                std::cout << it.key() << " : " << it.value() << '\n';
            }
        }
        // add-source [source name] [source url]
        if (find_and_remove(command, "add-source "))
        {
            //split the command at the ws character
            //open the config file and parse as json
            //add the first string (source name) and the second string (source url) to the json
            //write the json to file
        }

        


    }    
}




/* takes data from one source and sends it to the output url*/
void continue_streaming (InputStream& source, OutputStream& sink, 
                         std::chrono::_V2::steady_clock::time_point& end_time, ControlFlags& flags)
{
    while (flags.normal_streaming && !flags.stop)
    {
        try
        {
            source.get_one_output_frame();
            sink.write_frame(source);
            source.sleep(end_time);        
        }

        catch (const char* exception)
        {
            std::cout<<exception<<": changing to default source\n";
            source = InputStream {sink.get_output_codec_context(), DefaultSourceModes::white_noise};
        }   
    }    
}

/*takes data from the current and incoming sources, crossfades them and sends data to the output URL. 
* returns a (rvalue) reference to the incoming stream if the crossfade completes successfully 
* or a (rvalue) reference to the valid stream (immeadiately) in case of any errors*/
InputStream&& crossfade (InputStream& source, InputStream& new_source, 
                        OutputStream& sink, std::chrono::_V2::steady_clock::time_point& end_time, ControlFlags& flags)
{
    int fade_time_remaining = DEFAULT_FADE_MS;
    const int fade_time = DEFAULT_FADE_MS;
    try
    {
        source.init_crossfade();
        new_source.init_crossfade();
    }

    catch (const char* exception)
    {
        std::cout<<exception<<": crossfading failed\n";
        source.end_crossfade();
        return std::move(source);
    }
       
    while (fade_time_remaining > 0 && !flags.stop)
    {
        /*attempt to decode new input frame*/
        try
        {
            new_source.get_one_output_frame();
        }

        /*if incoming source fails, stop crossfading and return the old source*/
        catch(const char* exception)
        {
            std::cout<<"new source: "<<exception<<": crossfading failed \n";
            source.end_crossfade();
            return std::move(source);
        }
        
        /*attempt to decode input frame and crossfade together*/
        try
        {
            source.crossfade_frame (new_source.get_frame(), fade_time_remaining, fade_time);
        }

        /*if outgoing source fails, replace it with a silent source and continue crossfading*/
        catch (const char* exception)
        {
            std::cout<<"outgoing source: "<<exception<<": switching to default source for remaining fade duration\n";
            source = InputStream(sink.get_output_codec_context(), DefaultSourceModes::silence);
            continue;
        }

        sink.write_frame(source);
        source.sleep(end_time);
    }
    new_source.end_crossfade();
    return std::move(new_source);
}


bool find_and_remove(std::string& command, const std::string& substring)
{
    std::size_t found = command.find(substring);
    if (found != std::string::npos)
    {

    }
}