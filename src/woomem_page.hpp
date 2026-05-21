namespace woomem
{
    struct Page
    {
        static constexpr size_t NORMAL_PAGE_SIZE = 32768;
        char _reserved_[NORMAL_PAGE_SIZE];

        Page() = default;
        ~Page() = default;

        Page(const Page&) = delete;
        Page(Page&&) = delete;
        Page& operator = (const Page&) = delete;
        Page& operator = (Page&&) = delete;
    };
}