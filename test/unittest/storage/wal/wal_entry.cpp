
#include "base_test.h"
#include "main/infinity.h"
#include "main/logger.h"
#include "main/profiler/base_profiler.h"
#include "main/stats/global_resource_usage.h"
#include "storage/data_block.h"
#include "storage/txn/txn_manager.h"
#include <gtest/gtest.h>
#include <vector>

class WalEntryTest : public BaseTest {
    void
    SetUp() override {
        infinity::GlobalResourceUsage::Init();
        std::shared_ptr<std::string> config_path = nullptr;
        infinity::Infinity::instance().Init(config_path);
    }

    void
    TearDown() override {
        infinity::Infinity::instance().UnInit();
        EXPECT_EQ(infinity::GlobalResourceUsage::GetObjectCount(), 0);
        EXPECT_EQ(infinity::GlobalResourceUsage::GetRawMemoryCount(), 0);
        infinity::GlobalResourceUsage::UnInit();
        system("rm -rf /tmp/infinity/data/db");
        system("rm -rf /tmp/infinity/data/catalog/*");
        system("rm -rf /tmp/infinity/_tmp");
    }
};

using namespace infinity;

SharedPtr<TableDef>
MockTableDesc2() {
    // Define columns
    Vector<SharedPtr<ColumnDef>> columns;
    {
        i64 column_id = 0;
        {
            HashSet<ConstraintType> constraints;
            constraints.insert(ConstraintType::kUnique);
            constraints.insert(ConstraintType::kNotNull);
            auto column_def_ptr = MakeShared<ColumnDef>(
                    column_id++,
                    MakeShared<DataType>(DataType(LogicalType::kTinyInt)),
                    "tiny_int_col", constraints);
            columns.emplace_back(column_def_ptr);
        }
        {
            HashSet<ConstraintType> constraints;
            constraints.insert(ConstraintType::kPrimaryKey);
            auto column_def_ptr = MakeShared<ColumnDef>(
                    column_id++,
                    MakeShared<DataType>(DataType(LogicalType::kBigInt)),
                    "big_int_col", constraints);
            columns.emplace_back(column_def_ptr);
        }
        {
            HashSet<ConstraintType> constraints;
            constraints.insert(ConstraintType::kNotNull);
            auto column_def_ptr = MakeShared<ColumnDef>(
                    column_id++,
                    MakeShared<DataType>(DataType(LogicalType::kDouble)),
                    "double_col", constraints);
            columns.emplace_back(column_def_ptr);
        }
    }

    return MakeShared<TableDef>(MakeShared<String>("default"),
                                MakeShared<String>("tbl1"), columns);
}

TEST_F(WalEntryTest, ReadWrite) {
    SharedPtr<WalEntry> entry = MakeShared<WalEntry>();

    entry->cmds.push_back(MakeShared<WalCmdCreateDatabase>("db1"));
    entry->cmds.push_back(MakeShared<WalCmdDropDatabase>("db1"));
    entry->cmds.push_back(
            MakeShared<WalCmdCreateTable>("db1", MockTableDesc2()));
    entry->cmds.push_back(MakeShared<WalCmdDropTable>("db1", "tbl1"));
    entry->cmds.push_back(MakeShared<WalCmdAppend>("db1", "tbl1", nullptr));
    Vector<RowID> row_ids = {{1, 2}};
    entry->cmds.push_back(MakeShared<WalCmdDelete>("db1", "tbl1", row_ids));

    entry->cmds.push_back(MakeShared<WalCmdCheckpoint>(int64_t(123)));

    int32_t exp_size = entry->GetSizeInBytes();
    std::vector<char> buf(exp_size, char(0));
    char* buf_beg = buf.data();
    char* ptr = buf_beg;
    entry->WriteAdv(ptr);
    EXPECT_EQ(ptr - buf_beg, exp_size);

    ptr = buf_beg;
    SharedPtr<WalEntry> entry2 = WalEntry::ReadAdv(ptr, exp_size);
    EXPECT_NE(entry2, nullptr);
    EXPECT_EQ(*entry == *entry2, true);
    EXPECT_EQ(ptr - buf_beg, exp_size);
}
