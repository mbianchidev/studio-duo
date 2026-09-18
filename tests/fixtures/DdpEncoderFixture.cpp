#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    std::filesystem::path input;
    std::filesystem::path cue;
    std::filesystem::path output;
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string option(argv[index]);
        if (option == "--input-wav")
            input = argv[index + 1];
        else if (option == "--cue")
            cue = argv[index + 1];
        else if (option == "--output")
            output = argv[index + 1];
    }
    if (!std::filesystem::is_regular_file(input)
        || !std::filesystem::is_regular_file(cue)
        || output.empty())
    {
        std::cerr << "Missing input WAV, CUE, or output directory.\n";
        return 2;
    }

    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error)
    {
        std::cerr << error.message() << "\n";
        return 3;
    }
    std::filesystem::copy_file(
        input,
        output / "IMAGE.DAT",
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error)
    {
        std::cerr << error.message() << "\n";
        return 4;
    }
    std::filesystem::copy_file(
        cue,
        output / "PQ_DESCR",
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error)
    {
        std::cerr << error.message() << "\n";
        return 5;
    }
    std::ofstream(output / "DDPID")
        << "STUDIO DUO DDP ENCODER FIXTURE\n";
    std::ofstream(output / "DDPMS")
        << "IMAGE.DAT\nPQ_DESCR\n";
    return 0;
}
