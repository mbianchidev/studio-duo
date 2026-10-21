#include <cstdio>
#include <sys/stat.h>

int main(int argc, char* argv[])
{
    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/')
    {
        std::fprintf(stderr, "Usage: studio-duo-update-swap ABSOLUTE_CANDIDATE ABSOLUTE_TARGET\n");
        return 2;
    }
    struct stat candidate {}, target {};
    if (lstat(argv[1], &candidate) != 0 || lstat(argv[2], &target) != 0)
    {
        std::perror("Could not inspect the application directories");
        return 1;
    }
    if (!S_ISDIR(candidate.st_mode) || !S_ISDIR(target.st_mode)
        || candidate.st_dev != target.st_dev || candidate.st_ino == target.st_ino)
    {
        std::fprintf(stderr, "Update exchange requires distinct real directories on the same filesystem.\n");
        return 1;
    }
    if (renamex_np(argv[1], argv[2], RENAME_SWAP) != 0)
    {
        std::perror("Could not atomically exchange the application directories");
        return 1;
    }
    return 0;
}
