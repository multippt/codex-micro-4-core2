#include <unity.h>

#include "StickS3Controller.h"

using codex_micro::StickButtonController;
using codex_micro::StickGesture;
using codex_micro::StickPage;
using codex_micro::StickPageSelection;
using codex_micro::StickUiController;

void setUp() {}
void tearDown() {}

void test_single_click_is_delayed() {
  StickButtonController button;
  button.pressed(10);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::None),
                        static_cast<int>(button.released(20)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::None),
                        static_cast<int>(button.update(370)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::Next),
                        static_cast<int>(button.update(371)));
}

void test_double_click_suppresses_single() {
  StickButtonController button;
  button.pressed(10);
  button.released(20);
  button.pressed(100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::Previous),
                        static_cast<int>(button.released(120)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::None),
                        static_cast<int>(button.update(1000)));
}

void test_hold_suppresses_release() {
  StickButtonController button;
  button.pressed(100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::None),
                        static_cast<int>(button.update(599)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::NextPage),
                        static_cast<int>(button.update(600)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::None),
                        static_cast<int>(button.released(650)));
}

void test_timer_wraparound() {
  StickButtonController button;
  button.pressed(UINT32_MAX - 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickGesture::NextPage),
                        static_cast<int>(button.update(399)));
}

void test_page_counts_and_wrap() {
  StickUiController ui;
  TEST_ASSERT_EQUAL_UINT8(8, ui.itemCount());
  ui.move(-1);
  TEST_ASSERT_EQUAL_UINT8(7, ui.selection());
  ui.changePage(1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Commands),
                        static_cast<int>(ui.page()));
  TEST_ASSERT_EQUAL_UINT8(8, ui.itemCount());
  ui.changePage(1);
  TEST_ASSERT_EQUAL_UINT8(9, ui.itemCount());
  ui.changePage(1);
  TEST_ASSERT_EQUAL_UINT8(4, ui.itemCount());
  ui.changePage(1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Agents),
                        static_cast<int>(ui.page()));
}

void test_agent_selection_is_isolated_from_other_pages() {
  StickUiController ui;
  ui.noteAgent(4);
  TEST_ASSERT_EQUAL_UINT8(4, ui.selection());
  ui.changePage(1);
  ui.move(3);
  ui.noteAgent(2);
  TEST_ASSERT_EQUAL_UINT8(3, ui.selection());
  TEST_ASSERT_EQUAL_UINT8(2, ui.selectedAgent());
  ui.changePage(-1);
  TEST_ASSERT_EQUAL_UINT8(2, ui.selection());
}

void test_unpair_defaults_to_cancel() {
  StickUiController ui;
  ui.changePage(-1);
  ui.beginUnpairConfirmation();
  TEST_ASSERT_TRUE(ui.confirmingUnpair());
  TEST_ASSERT_FALSE(ui.confirmsUnpair());
  ui.move(1);
  TEST_ASSERT_TRUE(ui.confirmsUnpair());
  ui.cancelUnpairConfirmation();
  TEST_ASSERT_FALSE(ui.confirmingUnpair());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Config),
                        static_cast<int>(ui.page()));
  TEST_ASSERT_EQUAL_UINT8(0, ui.selection());
}

void test_next_arrow_persists_across_all_pages() {
  StickUiController ui;
  const uint8_t destinationCounts[] = {8, 9, 4, 8};
  for (uint8_t count : destinationCounts) {
    ui.changePage(1, StickPageSelection::NextArrow);
    TEST_ASSERT_EQUAL_UINT8(count, ui.itemCount());
    TEST_ASSERT_EQUAL_UINT8(count - 1, ui.selection());
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Agents),
                        static_cast<int>(ui.page()));
}

void test_previous_arrow_persists_across_all_pages() {
  StickUiController ui;
  const uint8_t destinationCounts[] = {4, 9, 8, 8};
  for (uint8_t count : destinationCounts) {
    ui.changePage(-1, StickPageSelection::PreviousArrow);
    TEST_ASSERT_EQUAL_UINT8(count, ui.itemCount());
    TEST_ASSERT_EQUAL_UINT8(count - 2, ui.selection());
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Agents),
                        static_cast<int>(ui.page()));
}

void test_default_page_change_selects_first_action() {
  StickUiController ui;
  ui.move(-1);
  ui.changePage(1);
  TEST_ASSERT_EQUAL_UINT8(0, ui.selection());
}

void test_user_selection_wins_after_queued_host_update() {
  StickUiController ui;

  // Apply the already-queued host selection before processing user input.
  ui.noteAgent(2);
  ui.changePage(-1, StickPageSelection::PreviousArrow);
  ui.changePage(1, StickPageSelection::PreviousArrow);
  ui.move(-1);

  // Activating Agent 6 does not advance or restore the older host selection.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(StickPage::Agents),
                        static_cast<int>(ui.page()));
  TEST_ASSERT_EQUAL_UINT8(5, ui.selection());
  TEST_ASSERT_EQUAL_UINT8(5, ui.selectedAgent());

  // A genuinely newer host update still retains the host-follow behavior.
  ui.noteAgent(4);
  TEST_ASSERT_EQUAL_UINT8(4, ui.selection());
  TEST_ASSERT_EQUAL_UINT8(4, ui.selectedAgent());
}

void test_urgent_state_classification() {
  TEST_ASSERT_FALSE(codex_micro::stickAgentUrgent(0));
  TEST_ASSERT_FALSE(codex_micro::stickAgentUrgent(1));
  TEST_ASSERT_TRUE(codex_micro::stickAgentUrgent(2));
  TEST_ASSERT_FALSE(codex_micro::stickAgentUrgent(3));
  TEST_ASSERT_TRUE(codex_micro::stickAgentUrgent(4));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_click_is_delayed);
  RUN_TEST(test_double_click_suppresses_single);
  RUN_TEST(test_hold_suppresses_release);
  RUN_TEST(test_timer_wraparound);
  RUN_TEST(test_page_counts_and_wrap);
  RUN_TEST(test_agent_selection_is_isolated_from_other_pages);
  RUN_TEST(test_unpair_defaults_to_cancel);
  RUN_TEST(test_next_arrow_persists_across_all_pages);
  RUN_TEST(test_previous_arrow_persists_across_all_pages);
  RUN_TEST(test_default_page_change_selects_first_action);
  RUN_TEST(test_user_selection_wins_after_queued_host_update);
  RUN_TEST(test_urgent_state_classification);
  return UNITY_END();
}
