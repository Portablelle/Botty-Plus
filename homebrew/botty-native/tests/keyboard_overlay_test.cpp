// SPDX-License-Identifier: GPL-3.0-or-later
// Include the real UI to exercise its modal dispatch through the host renderer.
#define main bottyNativeAppMain
#include "../src/main.cpp"
#undef main
#include <cassert>
extern "C" bool overlayImeFullMagnet;
namespace {
unsigned frame=0,retries=0;
bool overlayFrame(Canvas& canvas) noexcept {
    using Buttons=botty::Buttons;
    unsigned edge=0;
    if(frame==0){
        model.tab=Model::activity;pad=1;workflow.add();textEntryState=TextEntryState::ready;
        notify(Toast::Kind::error,"Request failed","Fixture failure");edge=Buttons::circle;
    }else if(frame==1){
        // A failure stays until Circle: a later success cannot replace it.
        notify(Toast::Kind::error,"Request failed","Fixture failure");notify(Toast::Kind::success,"Done","Fixture success");
        assert(toast.kind==Toast::Kind::error);toast.visible=false;
        notify(Toast::Kind::success,"Done","Fixture success");edge=Buttons::square;
    }
    else if(frame==2)edge=Buttons::circle;
    else if(frame==3&&!retries){
        workflow.search();std::snprintf(workflow.command.text.data(),workflow.command.text.size(),"original");
        assert(nativeKeyboard.open("initial","Search all trackers",200,false,false));
        assert(nativeKeyboard.poll()==botty::NativeKeyboard::Result::accepted&&nativeKeyboard.text()=="edited");
        textEntryState=TextEntryState::accepted;
        // An unknown or busy network snapshot keeps the accepted text for a later frame.
        const auto previousNotice=workflow.notice;
        assert(!finishNativeInput(0,false,false)&&workflow.notice==previousNotice);
        assert(!finishNativeInput(0,true));
        assert(textEntryState==TextEntryState::accepted&&nativeKeyboard.text()=="edited");
        assert(workflow.panel==Panel::keyboard&&std::string_view(workflow.command.text.data())=="original");
        notify(Toast::Kind::error,"Request failed","Fixture failure");
    }else if(frame==4){
        workflow.panel=Panel::keyboard;textEntryState=TextEntryState::idle;
        std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard closed.");
    }else if(frame==6&&!retries){
        workflow.add();textEntryState=TextEntryState::editing;overlayImeFullMagnet=true;
        assert(nativeKeyboard.open(workflow.command.text.data(),"Add a magnet link",16384,false,true));
    }
    input.head=0;input.count=edge?1:0;input.events[0]=edge;
    const auto previousDisplay=displayRevision;
    assert(draw(canvas));
    if(frame==0){
        // Circle dismisses a failure toast and nothing else.
        assert(!toast.visible&&workflow.panel==Panel::keyboard&&textEntryState==TextEntryState::ready);
        assert(std::string_view(workflow.command.text.data())=="magnet:?xt=urn:btih:");
    }else if(frame==1){
        // Toasts never freeze input: Square still selects the in-app keyboard.
        assert(toast.visible&&toast.kind==Toast::Kind::success&&textEntryState==TextEntryState::fallback&&workflow.panel==Panel::keyboard);
    }else if(frame==2)assert(workflow.panel==Panel::closed&&toast.visible);
    else if(frame==3){
        // A busy worker snapshot may defer acceptance; it must not drop the text.
        if(textEntryState==TextEntryState::accepted&&++retries<50)return true;
        assert(displayRevision>previousDisplay&&canvas.take_dirty());
        assert(textEntryState==TextEntryState::ready&&nativeKeyboard.text().empty()&&toast.visible);
        assert(workflow.panel==Panel::keyboard&&std::string_view(workflow.command.text.data())=="edited"&&workflow.notice[0]);
    }else if(frame==4)assert(nativeKeyboard.active()&&!workflow.notice[0]);
    else if(frame==5)assert(!nativeKeyboard.active());
    else if(frame==6){
        // The system cap may trim a pasted link: review it in the in-app editor.
        if(textEntryState==TextEntryState::capacity&&++retries<100)return true;
        assert(textEntryState==TextEntryState::fallback&&workflow.panel==Panel::keyboard);
        assert(std::string_view(workflow.command.text.data()).size()==2048&&workflow.notice[0]);
        assert(nativeKeyboard.text().empty());return false;
    }
    ++frame;retries=0;return true;
}
}
int main(){
    ps5::demo::run(overlayFrame,"Keyboard overlay test");
    std::puts("Toasts never freeze input; Circle only dismisses failures; pending IME acceptance survives busy snapshots; reopening clears stale notices.");
}
