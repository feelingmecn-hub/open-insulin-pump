# 保留 Hilt / 注入类
-keep class **HiltWrapper** { *; }
-keep class dagger.hilt.** { *; }
-keep class javax.inject.** { *; }

# Room
-keep class * extends androidx.room.RoomDatabase
-keep @androidx.room.Entity class *
-dontwarn androidx.room.paging.**

# Retrofit / Gson 模型
-keepattributes Signature
-keepattributes *Annotation*
-keep class com.openloop.pump.data.nightscout.model.** { *; }

# 保留 BLE / 协程
-dontwarn org.jetbrains.annotations.**
