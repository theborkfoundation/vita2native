/* Fixed-ratio frame skipping: one drawn frame in N, no timing involved. */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "frame_skip.h"

int main(void)
{
    int drawn = 0;

    setenv("VITA2NATIVE_FRAME_SKIP", "3", 1);

    for (int i = 0; i < 30; i++) {
        bool current = frame_skip_frame_current();

        if (current)
            drawn++;
        frame_skip_frame_end(!current);
    }

    assert(drawn == 10);
    assert(frame_skip_take_skipped() == 20);
    return 0;
}
