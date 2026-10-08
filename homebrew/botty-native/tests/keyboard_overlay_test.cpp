// SPDX-License-Identifier: GPL-3.0-or-later
// Include the real UI to exercise its modal dispatch through the host renderer.
#define main bottyNativeAppMain
#include "../src/main.cpp"
#undef main
#include <cassert>
namespace {
unsigned frame=0;
bool overlayFrame(Canvas& canvas) noexcept {
    using Panel=botty::Workflow::Panel;using Buttons=botty::Buttons;
    unsigned edge=0;
    if(frame==0){
        model.tab=0;pad=1;workflow.add();textEntryState=TextEntryState::ready;
        showResult=true;edge=Buttons::options;
    }else if(frame==1)edge=Buttons::cross;
    else if(frame==2){showResult=true;edge=Buttons::circle;}
    else if(frame==3){showResult=true;textEntryState=TextEntryState::fallback;edge=Buttons::square;}
    else if(frame==4)edge=Buttons::circle;
    else if(frame==5){
        workflow.command.operation=botty::Operation::extract;
        std::snprintf(workflow.command.text.data(),workflow.command.text.size(),"original");
        assert(nativeKeyboard.open("edited","Password",1024,true,false));
        textEntryState=TextEntryState::editing;showResult=true;edge=Buttons::circle;
    }else if(frame==6)edge=Buttons::cross;
    else if(frame==8){
        workflow.panel=Panel::keyboard;textEntryState=TextEntryState::idle;
        std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard closed.");
    }
    input.head=0;input.count=edge?1:0;input.events[0]=edge;
    const auto previousDisplay=displayRevision;
    assert(draw(canvas));
    if(frame<=4){
        assert(workflow.panel==Panel::keyboard&&std::string_view(workflow.command.text.data())=="magnet:?xt=urn:btih:");
        assert(!nativeKeyboard.active()&&(textEntryState==TextEntryState::ready||textEntryState==TextEntryState::fallback));
        assert(showResult==(frame==0||frame==3));
    }else if(frame==5||frame==6){
        assert(showResult==(frame==5));
        assert(textEntryState==TextEntryState::accepted&&nativeKeyboard.text()=="edited");
        assert(!finishNativeInput(0,true));
        assert(textEntryState==TextEntryState::accepted&&nativeKeyboard.text()=="edited");
        assert(workflow.panel==Panel::keyboard&&std::string_view(workflow.command.text.data())=="original");
    }else if(frame==7){
        assert(displayRevision>previousDisplay&&canvas.take_dirty());
        assert(textEntryState!=TextEntryState::accepted&&nativeKeyboard.text().empty());
        assert(workflow.panel==Panel::confirm&&!workflow.confirm&&std::string_view(workflow.command.text.data())=="edited");
    }else if(frame==8)assert(nativeKeyboard.active()&&!workflow.notice[0]);
    else if(frame==9){assert(!nativeKeyboard.active());return false;}
    ++frame;return true;
}
}
int main(){
    ps5::demo::run(overlayFrame,"Keyboard overlay test");
    std::puts("Result overlays freeze workflow input and preserve pending IME acceptance; reopening clears stale notices.");
}
