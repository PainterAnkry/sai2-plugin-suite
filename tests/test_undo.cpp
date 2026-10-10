// ============================================================================
// test_undo.cpp - UndoStack / UndoGroup（快照式撤销）
// ============================================================================

#include "Tests.h"
#include "sai2/host/UndoStack.h"

namespace sai2test {

bool TestUndo() {
    using namespace sai2;
    using namespace sai2::host;

    // ---- 单步快照撤销 ----
    {
        UndoStack stack;
        ImageBuffer img(4, 4, PixelFormat::RGBA8);
        img.Clear(RGBA{10, 10, 10, 255});

        stack.BeginGroup("Fill");
        stack.PushSnapshot("Fill", img);
        img.Clear(RGBA{200, 0, 0, 255});
        stack.EndGroup();

        CHECK(stack.CanUndo());
        CHECK(stack.LastGroupName() == "Fill");
        CHECK(stack.Undo(img));
        RGBA got;
        CHECK(img.GetPixel(0, 0, got) && got.r == 10 && got.g == 10);
        CHECK(!stack.CanUndo());
        CHECK(!stack.Undo(img)); // 空栈撤销失败
    }

    // ---- 组内多张快照：撤销恢复组内最早状态 ----
    {
        UndoStack stack;
        ImageBuffer img(4, 4, PixelFormat::RGBA8);

        stack.BeginGroup("BrushStroke"); // 模拟一次笔刷：多步快照
        img.Clear(RGBA{1, 1, 1, 255});
        stack.PushSnapshot("step1", img);
        img.Clear(RGBA{2, 2, 2, 255});
        stack.PushSnapshot("step2", img);
        img.Clear(RGBA{3, 3, 3, 255});
        stack.EndGroup();

        CHECK(stack.Undo(img));
        RGBA got;
        CHECK(img.GetPixel(0, 0, got) && got.r == 1); // 恢复到最早快照
        CHECK(!stack.CanUndo());                      // 整组只算一次撤销
    }

    // ---- 组外快照自动成为匿名单步组 ----
    {
        UndoStack stack;
        ImageBuffer img(2, 2, PixelFormat::RGBA8);
        img.Clear(RGBA{5, 5, 5, 255});
        stack.PushSnapshot("auto", img); // 不在组内
        img.Clear(RGBA{9, 9, 9, 255});
        CHECK(stack.Undo(img));
        RGBA got;
        CHECK(img.GetPixel(0, 0, got) && got.r == 5);
    }

    // ---- 多组 LIFO ----
    {
        UndoStack stack;
        ImageBuffer img(2, 2, PixelFormat::RGBA8);
        img.Clear(RGBA{1, 0, 0, 255});
        stack.BeginGroup("A");
        stack.PushSnapshot("A", img);
        img.Clear(RGBA{2, 0, 0, 255});
        stack.EndGroup();
        stack.BeginGroup("B");
        stack.PushSnapshot("B", img);
        img.Clear(RGBA{3, 0, 0, 255});
        stack.EndGroup();

        CHECK(stack.Size() == 2);
        CHECK(stack.LastGroupName() == "B");
        CHECK(stack.Undo(img));
        RGBA got;
        CHECK(img.GetPixel(0, 0, got) && got.r == 2);
        CHECK(stack.LastGroupName() == "A");
        CHECK(stack.Undo(img));
        CHECK(img.GetPixel(0, 0, got) && got.r == 1);
    }

    std::cout << "  undo: 单步/分组/多组 LIFO 全部通过\n";
    return true;
}

} // namespace sai2test
