#pragma once

#include <vector>

struct View;
struct Server;

// Layout box
struct Box {
    int x, y, w, h;
};

class Layout {
public:
    // Apply master-stack tiling to all mapped views within the given area
    void tile(std::vector<View *> &views, Box area, float master_ratio, int gap);
};
