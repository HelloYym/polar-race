package com.alibabacloud.polar_race.engine.common.utils;

import net.smacke.jaydio.DirectIoLib;
import net.smacke.jaydio.buffer.AlignedDirectByteBuffer;

/**
 * Created by IntelliJ IDEA.
 * User: wenchao.qi
 * Date: 2018/11/10
 * Time: 下午12:15
 */
public class ReadBuffer {
    private byte[] readBytes;
    private AlignedDirectByteBuffer readBuffer;

    public ReadBuffer(DirectIoLib directIoLib){
        readBytes = new byte[4096];
        readBuffer = AlignedDirectByteBuffer.allocate(directIoLib, 4096);
    }

    public byte[] getReadBytes() {
        return readBytes;
    }

    public AlignedDirectByteBuffer getReadBuffer() {
        return readBuffer;
    }
}
