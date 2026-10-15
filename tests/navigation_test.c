#include "../source/ui_navigation.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    UiNavigation n={0};
    assert(!ui_navigation_step(&n,4000,4000,0,0,0,0));
    assert(!ui_navigation_step(&n,11999,0,0,0,0,1));
    assert(ui_navigation_step(&n,12000,0,0,0,0,10)==UI_RIGHT);
    assert(!ui_navigation_step(&n,9000,0,0,0,0,11) && n.sticks[0]==UI_RIGHT);
    assert(!ui_navigation_step(&n,7999,0,0,0,0,12) && !n.sticks[0]);
    assert(!ui_navigation_step(&n,9000,0,0,0,0,13));
    assert(ui_navigation_step(&n,14000,0,0,0,0,100)==UI_RIGHT);
    assert(!ui_navigation_step(&n,14000,0,0,0,0,549));
    assert(ui_navigation_step(&n,14000,0,0,0,0,550)==UI_RIGHT);
    assert(!ui_navigation_step(&n,14000,0,0,0,0,729));
    assert(ui_navigation_step(&n,14000,0,0,0,0,730)==UI_RIGHT);
    assert(ui_navigation_step(&n,14000,0,0,0,0,5000)==UI_RIGHT);
    assert(!ui_navigation_step(&n,14000,0,0,0,0,5000));
    assert(!ui_navigation_step(&n,14000,0,0,0,0,5179));
    assert(!ui_navigation_step(&n,-9000,0,0,0,0,5180));
    assert(ui_navigation_step(&n,-12500,0,0,0,0,5181)==UI_LEFT);
    memset(&n,0,sizeof(n));
    assert(ui_navigation_step(&n,20000,15000,0,0,0,0)==UI_RIGHT);
    assert(!ui_navigation_step(&n,16000,17000,0,0,0,1) && n.sticks[0]==UI_RIGHT);
    assert(ui_navigation_step(&n,9000,22000,0,0,0,2)==UI_UP);
    memset(&n,0,sizeof(n));
    assert(ui_navigation_step(&n,0,0,0,20000,0,0)==UI_UP);
    assert(!ui_navigation_step(&n,0,0,0,10000,0,1) && n.sticks[1]==UI_UP);
    assert(!ui_navigation_step(&n,0,0,0,7999,0,2));
    assert(ui_navigation_step(&n,20000,0,0,-20000,0,3)==UI_RIGHT);
    assert(ui_navigation_step(&n,20000,0,0,-20000,UI_DOWN,10)==UI_DOWN);
    assert(!ui_navigation_step(&n,20000,0,0,-20000,UI_DOWN,409));
    assert(ui_navigation_step(&n,20000,0,0,-20000,UI_DOWN,410)==UI_DOWN);
    assert(!ui_navigation_step(&n,20000,0,0,-20000,UI_DOWN,509));
    assert(ui_navigation_step(&n,20000,0,0,-20000,UI_DOWN,510)==UI_DOWN);
    assert(!ui_navigation_step(&n,0,0,0,0,0,511));
    assert(ui_navigation_step(&n,INT_MIN,INT_MAX,0,0,0,512)==UI_LEFT);
    puts("Stick dead zone, release hysteresis, dominant axis, slow timed repeats, dual-stick arbitration and D-pad priority passed.");
    return 0;
}
