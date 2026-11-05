package com.yuutatsubasa.marbleknights;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Environment;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.TextView;
import java.io.File;
import java.util.Locale;

// Launcher activity: the 3.4 GB of StreamingAssets live in /sdcard/MarbleKnights/Raw (the player
// copies the folder the launcher made), so ask for storage access first, check that the data is
// there, then hand over to Unity. loader/proxy/proxy.c looks in the same place.
public class MkStartActivity extends Activity {
    static final String MARKER = "MarbleKnights/Raw/AssetBundles/ios/ios";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE }, 1);
        else
            proceed();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        proceed();
    }

    private boolean hasData() {
        File shared = new File(Environment.getExternalStorageDirectory(), MARKER);
        File own = getExternalFilesDir(null);
        return shared.exists() || (own != null && new File(own, "Raw/AssetBundles/ios/ios").exists());
    }

    private void proceed() {
        if (hasData()) {
            startActivity(new Intent(this, com.unity3d.player.UnityPlayerActivity.class));
            finish();
            return;
        }
        boolean zh = Locale.getDefault().getLanguage().equals("zh");
        boolean granted = checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
        String msg;
        if (!granted)
            msg = zh ? "Marble Knights 需要「檔案和媒體」權限，才能讀取放在手機裡的遊戲資料。\n\n請到 設定 → 應用程式 → Marble Knights → 權限 允許後，再開一次。"
                     : "Marble Knights needs the Files and media permission to read the game data on this phone.\n\nAllow it in Settings → Apps → Marble Knights → Permissions, then open the game again.";
        else
            msg = zh ? "找不到遊戲資料。\n\n請把啟動器做出來的「MarbleKnights」資料夾整個複製到手機內部儲存空間的最上層（跟 Download、DCIM 同一層），再開一次。"
                     : "The game data was not found.\n\nCopy the whole \"MarbleKnights\" folder that the launcher made to the top of this phone's internal storage (next to Download and DCIM), then open the game again.";
        TextView t = new TextView(this);
        t.setText(msg);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        t.setGravity(Gravity.CENTER);
        int pad = (int)(32 * getResources().getDisplayMetrics().density);
        t.setPadding(pad, pad, pad, pad);
        setContentView(t);
    }
}
