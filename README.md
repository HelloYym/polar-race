## PolarDB 比赛攻略

### 1 背景分析

#### 1.1 赛题概述

赛题：**https://code.aliyun.com/polar_race2018/competition_rules**

比赛要求实现一个简化、高效的基于 SSD 的 kv 存储引擎，支持 Write、Read、Range 接口。

```c++
RetCode Write(const PolarString &key, const PolarString &value);
RetCode Read(const PolarString &key, std::string *value);
RetCode Range(const PolarString &lower, const PolarString &upper, Visitor &visitor);
```

评测程序分为 2 个阶段：

1. 正确性评测阶段

   此阶段评测程序会并发写入特定数据（key 8B、value 4KB）同时进行任意次kill -9来模拟进程意外退出，接着重新打开 DB，调用 Read 和 Range 接口来进行正确性校验。

2. 性能评测阶段

   - 随机写入：64 个线程并发随机写入，每个线程使用 Write 各写 100 万次随机数据（key 8B、value 4KB）

   - 随机读取：64 个线程并发随机读取，每个线程各使用 Read 读取 100 万次随机数据

   - 顺序读取：64 个线程并发顺序读取，每个线程使用 Range 全局顺序迭代 DB 数据 2 次

#### 1.2 题目重点

基于题目的描述，可以发现本次比赛并不是要求实现一个通用的 kv 数据库，而是在一些方面做了简化和限定。因此可以充分的利用题目中的这些特点进行针对性的设计，我们总结了如下几个需要考虑的重点：

- **kv 定长**：key 为固定的 8 字节，value 为固定的 4KB，定⻓存储有利于⽂件和内存的管理以及索引的定位操作，例如，索引可以只记录数据的逻辑偏移量，而不需要记录实际的物理位置，这样节省了索引的内存开销。
- **value 远大于 key**：对于 value 数据比 key 大很多的情况，将 key 和 value 分离存储，可以解除索引构建和数据存储之间的耦合性，否则操作索引会产生很严重的读写放大。
- **分阶段测评**：随机写、随机读、顺序读三个阶段互相没有重叠，每个阶段结束都会关闭数据库，不存在同时读写的情况，所以写入阶段不需要建立内存索引。
- **线程数固定**：测评程序固定使用 64 个线程访问数据库。
- **key 随机分布**：根据线上表现，我们发现 key 是基本随机分布的长度为 8 的字符串。
- **kill -9 数据持久化**：只需保证进程意外退出时数据持久化不丢失，不要求保证在系统崩溃时的数据持久化不丢失，因此可以利用操作系统的缓存对写入方式进行一些优化。

#### 1.3 SSD 特性：并行、对齐、随机写

###### 1.3.1 基于 NAND 的闪存

基于 NAND 的闪存是一种非易失性的 EEPROM 存储设备，闪存芯片的最小读写单元是页（page，通常 4KB），多个连续的页组成了块（block），闪存的擦除操作按块进行。图 1-a 给出了一个闪存芯片结构的示意图，其中每个块包含 128 个 4 KB 的页。

<img src="https://ws3.sinaimg.cn/large/006tNc79ly1fyto2equltj314i0hkdh0.jpg" alt="图 1" width="75%">

闪存与传统存储介质有以下几点差异：（1）读和写具有不同的延迟，写的代价高了一个数量级。（2）不支持原地写回，如果一个数据页中已经有数据了，只有将该页所属的块整体擦除，新的数据才能写入这个页。（3）每个存储单元只有有限的擦写寿命。

###### 1.3.2 固态硬盘 SSD

固态硬盘通常由主机接口、闪存阵列、RAM 和 SSD 控制器组成，如图 1-b 所示。SSD 控制器的主要作用是将外部的读写请求转换为对闪存芯片的操作，并利用 RAM 对读写数据进行缓存，这些操作由控制器中的闪存转换层（FTL）固件来管理。

为了提高 SSD 的读写带宽，通常在硬件和 FTL 上使用一种交叉（interleaving）技术。一个写操作被分为两个步骤完成：（1）将数据装入闪存芯片内部的页寄存器；（2）将已经装载的数据编程写入对应的闪存页单元。因为数据编程写入闪存单元比装入寄存器需要更多的时间，因此在编程写入闪存单元的同时，可以在其它闪存芯片上载入数据。图 2 展示了一个 4-路交叉写入的情况，这种技术隐藏了闪存编程写入的延迟。如果 SSD 中有多个独立的闪存阵列通道，那利用它们的并行性可以极大地提高 SSD 的性能。

<img src="https://ws4.sinaimg.cn/large/006tNc79ly1fyto56x2plj30ne0bcdg4.jpg" alt="图 1" width="50%">

###### 1.3.3 闪存转换层（FTL）

闪存转换层是 SSD 上的主要控制软件，它隐藏了闪存独特的性质，模拟了与传统块设备相同的外部主机接口。FTL 最主要的作用是将外部的写操作请求重定位，写入到一个已经被擦除的空白的区域。因此 FTL 需要维护一个地址映射表，将文件系统的逻辑数据块地址（LBA）转换为内部的物理地址。为了快速的访问，在每个闪存页中存储它们对应的逻辑地址，每次启动时构建地址映射表，并将映射表装载到易失性存储介质中。

FTL 的地址映射可以是页级别的，也可以是块级别的。页级别的映射可以有效地解决先擦后写的限制，因为一个对页的请求可以被重定向到闪存中任意的空白页，如果一个块中有 N 个页，那 N 次写操作之后平均只需要一次块擦除操作，但是这种页级别的映射导致映射表太大，而无法装入易失性存储介质中。如果是块级别的映射，物理块中的页的偏移一定要与逻辑块中该页的偏移相同，否则将无法对页进行定位。然而，当更新一个页的内容时，必须写入一个空白块中相同的页，而原来块中其余的页也要被拷贝到新的块中，这样一次写请求产生了一个块擦除和 N 个页的写入操作。

为了克服这种技术问题，混合的 FTL 映射方式结合了页级别和块级别的映射。这种方法划分了一部分闪存块作为日志块，外部的写操作都是直接写入日志块，并且日志块的地址映射是页级别的，因此可以避免频繁的块擦除操作。其余的闪存块作为数据块，采用块级别的映射方式，通常数据块的数量远大于日志块的数量。每个日志块对应一个数据块，对该数据块的写操作被写入该日志块，当一个日志块中的所有页都被写入，则将它与对应的数据块进行合并。当没有空白的日志块可以处理写请求时，就需要选取一个没有完全填充的日志块进行释放，即将该日志块的内容与对应的数据块合并。日志块与数据块合并的过程就是将每个页的可用版本拷贝到一个新的数据块，并擦除原来的日志块和数据块，因此一个合并操作产生了两次擦除操作。理想的情况是，如果一个日志块包含了所有可用的页，那可以简单的将这个日志块标记为新的数据块，这样就只需要一次擦除原来数据块的操作，这种情况被称为切换合并（switch merge）。

###### 1.3.4 随机写的影响

SSD 内的闪存阵列通过多条总线连接在控制器上，并且每条总线上的闪存芯片可以实现 interleaving 技术，为了利用这种固有的并行性，SSD 通常将来自不同闪存芯片上的多个页组合为一个内部的读写单元，称为组合页（clustered page）。组合页的大小对于 I/O 性能具有非常重要的影响，如果写请求只更新组合页中的一部分，那控制器需要将原始组合页中其余的部分读出来，与请求更新的部分合并之后再写入新的组合页，如图 3-a 所示。因此，这种 read-modify-write 操作导致了额外的代价，增加了写操作的延迟。

为了提升垃圾回收的性能，与组合页类似的，SSD 将不同闪存芯片上的多个块组成内部的擦除单元，因此可以并行地擦除多个物理块。进行垃圾回收时，如果一个组合块中只有部分页是失效的，那要将其余的页拷贝到 SSD 中的空闲区域，这种复制代价降低了 SSD 垃圾回收的性能，就是说组合块的内部碎片导致了性能的下降。

<img src="https://ws1.sinaimg.cn/large/006tNc79ly1fytnimuqf3j30vc0msair.jpg" alt="图 1" width="70%">

我们考虑什么情况下会产生这种组合块的内部碎片。首先考虑图 3-b 中的第一种情况，写操作的请求大小小于组合页的大小，假定垃圾回收进程选择了最左侧的一个块进行回收。当数据被顺序地写入时，该块的全部数据被更新，因此该块没有可用数据，垃圾回收除了擦除该块外不需要额外代价。但是，如果随机写入情况下，该块只有部分数据被更新，产生了内部碎片，因此垃圾回收进程要将其余的数据拷贝出去，降低了写操作的性能。

接下来考虑图 3-b 中的后两种情况，如果写操作请求的大小是组合页大小的倍数，那每次随机写会整体更新组合块，并使得组合块中的全部数据失效，垃圾回收时只需要擦除选择的组合块而没有额外的代价，与顺序写的代价相同。在 FTL 的混合映射方式下，对应了切换合并（switch merge）这种最理想的情况。

综上所述，因为小尺寸的随机写导致 SSD 的内部碎片，产生额外的垃圾回收代价。因此，SSD 上的存储引擎设计应该充分考虑这种 I/O 模式的影响，增加写操作的局部性，或者使用更大的读写单元与组合页对齐。

#### 1.4 直接 I/O 和 mmap

###### 1.4.1 块设备读写与 bio 机制

块设备读写位于文件系统与驱动程序之间，文件系统发起的块读写请求经过块设备读写模块，然后发送到块设备驱动程序，块设备读写的性能直接影响到整个系统的性能。

bio 机制位于 Linux 读写层，基本上所有的读写操作都通过 bio 层实施（Direct I/O 除外），Linux 2.6 版本中将 bio 结构体独立出来用于描述一个读写操作。比如，一个读/写操作被转化为一个 bio，由这个 bio 转成一个或多个读/写 request，所有对某个设备的 request 放成一个队列 queue。

bio 层能对这些 request 进行排序和合并，排序后能方便磁盘读写，减少磁盘调度时间，也方便块设备层进行预读等操作；对 request 进行合并，能尽量地在一次读写中访问尽可能大的数据块，这样能发挥块设备的最大性能。为了对这些 request 进行排序合并，又衍生出各种 I/O 调度算法以追求达到最大的性能和吞吐量。

###### 1.4.2 缓存 I/O 与 page cache

page cache 是 Linux 的缓存系统，基于这样的思想而来：在一个系统中，空闲着的内存放在那里就是浪费，不如用它来缓存一些已经从磁盘上读取出来的数据，这样能大大提升系统的性能。当系统需要空闲内存的时候，我们再释放一部分缓存给内存分配系统。

缓存 I/O（Buffered I/O）又被称作标准 I/O，大多数文件系统的默认 I/O 操作都是缓存 I/O。在缓存 I/O 机制中，操作系统会将 I/O 的数据缓存在文件系统的页缓存（ page cache ）中。

由于 page cache 机制的存在，每次当内核需要读取文件数据的时候，检查对应的数据是否已经在 page cache 中，如果存在，那么可以直接从 cache 提供数据，而无需实际的物理读盘操作。由于应用程序倾向于重复使用数据块，这样 page cache 就大大提升了系统性能。

对于写操作来说，内核会将数据先写到 page cache 中去，数据是否被立即写到磁盘上去取决于应用程序所采用的写操作机制：

- 同步写机制（synchronous writes）：那么数据会立即被写回到磁盘上，应用程序会一直等到数据被写完为止；
- 延迟写机制（deferred writes）：应用程序不需要等到数据全部被写回到磁盘，数据只要被写到页缓存中去就可以了，操作系统会定期地将 page cache 刷盘，在数据完全写到磁盘上的时候不会通知应用程序，所以延迟写机制存在数据丢失风险。
- 异步写机制（asynchronous writes）：异步写机制在数据完全写到磁盘上的时候会执行应用程序的回掉函数，所以异步写机制不会有数据丢失风险。

在缓存 I/O 机制中，数据在传输过程中需要在用户地址空间和 page cache 之间进行多次数据拷贝操作，而不能直接在用户地址空间和磁盘之间进行数据传输，这些数据拷贝操作所带来的 CPU 以及内存开销是非常大的。

因为缓存 I/O 会对 page cache 进行预读，对于数据库这样的随机读写程序，预读的命中率会非常低，因此在随机读的场景下，page cache 会对性能造成很大的负面影响。

###### 1.4.3 直接 I/O

Linux 中的直接 I/O（Direct I/O）省略掉操作系统内核缓冲区的使用，数据直接在应用程序地址空间和磁盘之间进行传输，不需要 page cache 的支持，从而降低了文件读取和写入时 CPU 的利用率，也避免了随机读情况下的命中率低的问题。

Linux 内核支持了直接 I/O 方式，进程在打开文件的时候设置对文件的访问模式为 O_DIRECT，启用文件的直接 I/O 访问，接下来使用 read() 或者 write() 系统调用去读写文件使用的是直接 I/O 方式，所传输的数据均不经过内核缓存。使用直接 I/O 读写数据必须要进行缓冲区对齐，即写入数据大小必须是文件系统块大小的整数倍，同时写入数据地址与文件系统块大小对齐。

###### 1.4.4 mmap 内存文件映射

mmap 把文件映射到用户空间里的虚拟内存，省去了从内核缓冲区复制到用户空间的过程，文件中的位置在虚拟内存中有了对应的地址，可以像操作内存一样操作这个文件，相当于已经把整个文件放入内存，但在真正使用到这些数据前却不会消耗物理内存，也不会有读写磁盘的操作。

Linux 中提供了系统调用 mmap() 来实现这种文件访问方式。与标准的访问文件的方式相比，内存映射方式可以减少标准访问文件方式中系统调用次数，并减少数据在用户地址空间和内核地址空间之间的拷贝操作。映射通常适用于较大范围，对于相同长度的数据来讲，映射所带来的开销远远低于 CPU 拷贝所带来的开销。当大量数据需要传输的时候，采用内存映射方式去访问文件会获得比较好的效率。

### 2 核心思路

#### 2.1 数据分段

由于 key 字符串的分布是均匀的，我们根据 key 的前 12 位将全部 6400 万条数据划分到 4096 个分区，平均每个分区中数据量为 6400w / 4096 = 15625 条。根据字符串大小顺序的定义，4096 个分区之间是有序的。对数据进行分区会带来以下几个好处：

- 降低冲突：将每个分区的数据单独存储，在写入数据时，每个分区具有一个锁控制并发写入的一致性。相比于不做分区的情况，理论上锁的冲突降低了 4096 倍。
- 并行计算：可以使用多线程并发地排序多个分区的索引，使得索引构建时间不再是一个重点问题。
- 快速定位：在根据 key 查找数据时，可以根据 key 直接定位到所在分区，然后在对应分区中进行查找。
- 加载分区：可以将整个分区的数据完全加载进内存，读大块数据的操作有利于发挥 SSD 性能。 

数据分区的方法本质上属于利用空间换时间，假设我们使用 key 的前 64 位进行划分，那每个分区最多有 1 个数据，这样就不再需要查找数据的时间，同时也不存在锁冲突。不过由于 2^64 个分区中很多是没有数据的，我们没有足够的内存空间开辟这么大的数组，因此这样是不可行的。我们经过多次线上测试，综合多个因素，最终使用 4096 的分区数量。

#### 2.2 key/value 分离

对于 value 数据相比 key 比较大的情况，将 key 和 value 分离存储，可以解除索引构建和数据存储之间的耦合性：

- value 数据以 log 的形式存储于硬盘，一次顺序写入后不再需要移动，符合 SSD 顺序写的特性。
- 索引可以全部加载到内存，格式为 <key, vLog-offset>，在内存中可以快速排序并读取索引，其中，vLog-offset 是 value 在 valuelog 中的顺序。
- 恢复阶段只需读取体积很小的索引文件，不访问较大的 value 文件。

由于 key 和 value 不同时写入文件，可能会导致进程退出时的数据不一致。我们的策略是先写 value 文件，最后写 key 文件，open 阶段根据 key 文件中 key 的个数设置 value 文件指针，这样就确保了 open 成功后 key 和 value 的个数相同，解决了一致性问题。

#### 2.3 读写方式

key 的大小为固定 8 字节，对于这种小数据量的读写模式，采用 mmap 方式可以利用 page cache 将小数据量读写转换为整个内存页的读写，这样大块读写可以充分发挥 SSD 的性能。同时，相比于每次调用 read/write 函数，读写 mmap 用户内存区域不需要系统调用，大大减少了系统调用的时间消耗。

由于题目仅要求保证 kill -9 杀死进程时的数据一致性，mmap 可以利用系统的 page cache，在进程结束后由操作系统将内存数据刷盘。

value 的大小为固定 4KB，我们知道，写入数据大小是 SSD 组合页（Clustered Page）大小的整数倍时，可以达到 SSD 的最优写入性能。经过线上测试，在固定 64 线程写入的条件下，按 16KB 大小写入数据可以达到最大IOPS。

所以，我们为每一个分区分配一个容量为 16KB / 4KB = 4 的 value 写入缓存，写满 4 个 value 时将 16KB 数据一起刷盘，刷盘方式采用 DirectIO，这样可以绕开操作系统的 page cache，避免了数据到 page cache 的拷贝。

为了保证 kill -9 时的数据一致性，我们考虑用 mmap 做写缓冲区，这样在 kill -9 之后，缓冲区数据会被操作系统写回文件，在下次 open 时将该文件重新 mmap 就完成了缓冲区的恢复。

对于 value 数据读取，我们依然采用 DirectIO 方式，这样可以避免操作系统预读过多无用数据到 page cache 浪费带宽。

#### 2.4 文件架构

由于每个数据分段的数据量近似相等，因此我们使用定长文件存储 key 和 value。预先分配每个分区的容量为 2^14 个数据，在某些分段数据量超出容量后，可以重新分配空间进行自动扩容。

如果将每个分区单独存储，那我们需要 4096 * 2 个文件，在实际测试中，太多的文件数导致了写入速度变慢。因此，考虑在一个文件中存储多个分区的数据，但这样每个文件都是随机写的模式。根据上面提到的 SSD 的性质，当写入数据量大于 SSD 组合页大小时，随机写入速度与顺序写入速度相同。因此，使用一个文件存储多个数据分段的方法不会影响性能。

通过线上测试发现，多线程同时读单一文件会导致性能下降，我们猜测是这种读模式没有充分利用 SSD 的并行性所导致的。如果相邻分片在同一个文件中，会影响 range 阶段同时访问相邻分片时的性能。因此，我们采用了如下所示的文件架构。

valueFiles 由 64 个⽂件组成，分⽚大小及规则如下图：

<img src="https://ws3.sinaimg.cn/large/006tNc79ly1fyto9k13mrj318u0ncmyl.jpg" alt="图 1" width="60%">


其中 keyFiles 由 64 个文件组成，包括 key 部分和 16KB 写缓存部分，分⽚⼤小及规则如下图：

<img src="https://ws2.sinaimg.cn/large/006tNc79ly1fytoafrculj31ag0k2407.jpg" alt="图 1" width="80%">


#### 2.5 索引构建和查询

由于在写入数据时没有进行索引构建，因此在每次 open 数据库时判断数据文件是否存在，如果存在则进行内存索引的构建。

为了充分节省内存空间，我们考虑将索引项 <key, vLog-offset> 载入内存数组，然后进行原地排序并去重。这个过程使用 64 个线程并发地对 4096 个分区索引进行快速排序，排序时间复杂度 O(nlog n)，之后根据 key 进行去重，排序时间复杂度 O(n)。

排序后每个分区内有序，同时分区间有序。

进行点查询（point query）时，先根据 key 的前 12 位以 O(1) 的时间复杂度定位到所在分区，然后在对应分区中进行二分查找，二分查找时间复杂度为 O(log n)。

对于区间查询（range query），同样根据最小 key 值定位到起始分区，在该分区中二分查找起始位置，然后从起始位置开始顺序扫描后续的索引项，到达分区末尾时根据分区顺序继续扫描下一个分区，直到当前扫描的 key 值大于查询范围。

#### 2.6 平衡 I/O 和 CPU 负载

线上测试，将读取索引文件和排序两个阶段混合在一起的用时为 250ms 左右。读取索引文件主要占用 I/O 资源，排序主要占用 CPU 资源，我们认为，将两个阶段混合在一起不能完全占满 CPU 和 I/O 资源。

将读文件和排序分为两个独立的阶段，第一阶段索引项载入内存时间为 190ms 左右，第二阶段排序时间为 200ms 左右。

索引项载入阶段的 CPU 比较空闲，因此分配 1/2 的排序任务到索引项载入阶段，从而达到平衡 I/O 和 CPU 负载的目的，这样该阶段的时间还维持在 190ms 左右不变。

由于第二阶段只剩 1/2 的排序任务，所以排序时间降低到 100ms，在这 100ms 之中，I/O 处于空闲状态，根据测试可以预先读取 4 个文件数据块到 buffer，节省了后续阶段读取文件的时间。

#### 2.7 生产者/消费者模型

range 阶段 64 个线程从 buffer 获取数据，n 个读磁盘线程从文件加载数据到 buffer，同时缓冲区大小受限。这是经典的生产者/消费者模型，缓冲区的实现是一个循环队列，我们使用条件变量（Condition Variable）来实现线程间的协作。

如果生产者/消费者模型中的每个数据对应于一个 4KB 的 value 数据，那么此时还是小数据量的随机读磁盘模式，这样无法发挥 SSD 的最大性能，

生产者/消费者模型中的每个数据对应于我们的一个分片，因此，设置一个分片数据的缓冲区，每个缓冲区的大小为 64M。剩余内存空间为 1G 左右，可以开辟 16 个缓冲区，其中前 8 个缓存区是可替换的，后 8 个缓存是不可替换的，只能写入一次，这样第二次 range 访问到这 8 个缓冲区时就不需要读磁盘。range 阶段的内存缓冲区模型如下所示：

<img src="https://ws4.sinaimg.cn/large/006tNc79ly1fytoef9xodj31a40tkdhm.jpg" alt="图 1" width="60%">

其中 active 部分为 Ring Buffer，prepage 部分为 open 阶段为了平衡 I/O 和 CPU 负载所载入的数据分片，reserve 部分是保留的。

range 开始时，启动若干读磁盘线程（最终为 2 个线程），每个读磁盘线程会不断获取当前需要读的分片，然后判断缓存是否可⽤（是否被 64 个线程都 visit 结束），如果可⽤则读磁盘数据并写入缓冲区，否则等待。 

64 个 range 线程会依次 visit 每个分片，先判断当前分⽚是否在缓存中， 如果没有则等待。在 visit 一个分片时，由于分片索引中的 key 已经有序，只需依次取出 key 和 offset，并根据 offset 从缓存中读取相应的 value 即可。

### 3 关键代码

#### 3.1 随机写流程

###### 3.1.1 建立文件和内存映射

建立 64 个 KVFiles 对象，每个对象包含一个 value log 文件、一个用于 key log 和 value cache 的 mmap 文件。文件的打开方式使用直接 I/O 模式，然后初始化文件大小，并做内存文件映射。

```c++
//value log 文件 
//打开文件设置访问模式为 O_DIRECT，启用文件的直接 I/O 访问
this->valueFd = open(fp.str().data(), O_CREAT | O_RDWR | O_DIRECT, 0777);

//key log 和 value cache 文件
this->mapFd = open(mp.str().data(), O_CREAT | O_RDWR | O_DIRECT, 0777);

//初始化文件大小
ftruncate(this->mapFd, keyFileSize + blockFileSize);

//映射 key log 文件区域到用户地址空间
this->keyBuffer = static_cast<u_int64_t *>(mmap(nullptr, keyFileSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_NONBLOCK, this->mapFd, 0));

//映射 value cache 文件区域到用户地址空间
this->blockBuffer = static_cast<char *>(mmap(nullptr, blockFileSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_NONBLOCK, this->mapFd, keyFileSize));
```

###### 3.1.2 划分文件分片

根据 2.4 中所述的文件架构指定每个分片所在的文件和文件中偏移，并建立 4096 个 KeyValueLog 对象和 SortLog 对象，KeyValueLog 处理所有文件的读写请求，SortLog 负责索引的排序和查找。

```c++
for (int logId = 0; logId < LOG_NUM; logId++) {
    int fileId = logId % FILE_NUM;
    int slotId = logId / FILE_NUM;
    keyValueLogs[logId] = new KeyValueLog(path, logId, fileId, 
                                          slotId * VALUE_LOG_SIZE, 
                                          slotId * BLOCK_SIZE, 
                                          slotId * NUM_PER_SLOT);
    sortLogs[logId] = new SortLog(SORT_LOG_SIZE * logId);
}
```

###### 3.1.3 数据写入

根据 key 的前 12 位划分分片。

```C++
int getLogId(const char *k) {
    return ((u_int16_t) ((u_int8_t) k[0]) << 4) | ((u_int8_t) k[1] >> 4);
}
```

将 key 和 value 顺序写入对应文件，利用互斥锁避免多个线程写同一个分区时的冲突。

```C++
void put(const PolarString &key, const PolarString &value) {
    auto logId = getLogId(key.data());
    logMutex[logId].lock();
    keyValueLogs[logId]->putValueKey(value.data(), key.data());
    logMutex[logId].unlock();
}
```

先将 value 写入缓存，凑齐 4 个 value 时再一起写入 value 文件。

```C++
inline void putValueKey(const char *value, const char * key) {
    
    //将 value 数据复制到 value cache，并更新缓存的数据个数
    memcpy(cacheBuffer + ((cacheBufferPosition++) << 12), value, 4096);
    
    //如果缓存已满，将缓存数据写入磁盘文件，更新文件指针和缓存个数
    if (cacheBufferPosition == PAGE_PER_BLOCK) {
        pwrite(this->fd, cacheBuffer, BLOCK_SIZE, globalOffset + filePosition);
        filePosition += BLOCK_SIZE;
        cacheBufferPosition = 0;
    }
    
    //将 key 写入 mmap 内存区域，更新位置指针
    *(keyBuffer + keyBufferPosition++) = *((u_int64_t *) key);
}
```

#### 3.2 恢复流程

启动 64 个线程读取 key 文件内容，同时将生成的 <key, offset> 索引项存入对应 SortLog 对象中的数组，一个分片被读完后直接对索引项数组进行快速排序。

```C++
std::thread t[RECOVER_THREAD];
for (int i = 0; i < RECOVER_THREAD; i++) {
    t[i] = std::thread([i, this] {
        for (int logId = i, u_int64_t k; logId < LOG_NUM; logId += RECOVER_THREAD) {
            //读取 key 文件，生成索引项
            while (keyValueLogs[logId]->getKey(k))
                sortLogs[logId]->put(k);
            //对索引项数组排序
            sortLogs[logId]->quicksort();
            //恢复文件指针和内容
            keyValueLogs[logId]->recover((size_t) sortLogs[logId]->size());
        }
    });
}
```

在恢复数据库时，value cache 文件中的内容可能是上次进程 kill -9 之后没被写入 value 文件的数据，因此要对这部分数据进行刷盘。

```C++
void recover(u_int32_t sum) {
    this->filePosition = (off_t) sum << 12;
    this->cacheBufferPosition = sum % PAGE_PER_BLOCK;
    auto offset = (size_t) cacheBufferPosition << 12;
    filePosition -= offset;
    pwrite(this->fd, cacheBuffer, offset, globalOffset + filePosition);
}
```

#### 3.3 随机读流程

在分片的索引数组中二分查找 key 所在位置，并返回 value 在实际文件中的偏移。

```C++
RetCode read(const PolarString &key, string *value) {
    auto logId = getLogId(key.data());
    auto index = sortLogs[logId]->find(*((u_int64_t *) key.data()));

    if (index == -1) {
        return kNotFound;
    } else {
        auto buffer = readBuffer.get();
        keyValueLogs[logId]->readValue(index, buffer);
        value->assign(buffer, 4096);
        return kSucc;
    }
}
```

利用 thread_local 为每个线程分配一个独有的读缓冲区，并将缓冲区对齐文件系统页大小，用于直接 I/O。

```C++
static thread_local std::unique_ptr<char> readBuffer(static_cast<char *> (memalign((size_t) getpagesize(), 4096)));
```

#### 3.4 顺序读流程

###### 3.4.1 消费者线程

64 个线程同时顺序遍历全量数据，作为消费者的角色，每次从缓冲区请求一个分片数据，如果该分片不在缓冲区中，则该线程调用条件变量的 wait 函数等待，直到读磁盘线程将该分片读入缓冲区并发出 notify 信号通知该分片可读。

每个 range 线程访问完一个分片，将该分片的访问计数加一，当一个分片的访问计数达到 64 时，说明该分片已经被全部 64 个线程访问过，那么这个分片数据可以从缓冲区中删除，为后面要读入的数据腾出空间，同时发出 notify 信号通知读磁盘线程该缓冲区位置可用。

```C++
void range(Visitor &visitor) {
    //第一个调用 range 的线程负责启动 n 个读磁盘线程
    if (!readDiskFlag.test_and_set()) {
        for (int i = 0; i < READDISK_THREAD; i++) {
            std::thread(&PEngine::readDisk, this).detach();
        }
    }

    for (int logId = 0; logId < LOG_NUM; logId++) {
        //计算分片对应的缓冲区位置
        auto cacheIndex = logId % ACTIVE_CACHE_NUM;
        
        // 等待读磁盘线程将该分片读入缓冲区
        if (!isCacheReadable[cacheIndex] || currentCacheLogId[cacheIndex] != logId) {
            readDiskFinish[cacheIndex].lock();
            while (!isCacheReadable[cacheIndex] || currentCacheLogId[cacheIndex] != logId) 
                readDiskFinish[cacheIndex].wait();
            readDiskFinish[cacheIndex].unlock();
        }

        //读取缓冲区数据并调用 visit 函数
        auto cache = valueCache + cacheIndex * CACHE_SIZE;
        auto sortLog = sortLogs[logId];
        for (int i = 0, total = sortLog->size(); i < total; i++) {
            auto k = sortLog->findKeyByIndex(i);
            auto offset = sortLog->findValueByIndex(i) << 12;
            visitor.Visit(PolarString(((char *) (&k)), 8), PolarString((cache + offset), 4096));
        }

        //该缓冲区已经被全部 64 个线程访问过
        if (++rangeCacheCount[cacheIndex] == 64) {
            rangeCacheFinish[cacheIndex].lock();
            isCacheWritable[cacheIndex] = true;
            isCacheReadable[cacheIndex] = false;
            rangeCacheCount[cacheIndex] = 0;
            rangeCacheFinish[cacheIndex].notify_all();
            rangeCacheFinish[cacheIndex].unlock();
        }
    }
}
```

###### 3.4.2 生产者线程

启动 n 个读磁盘线程，作为生产者的角色，每个线程不断申请写缓冲区位置，拿到缓冲区位置后，需要等待该缓冲区位置变为空闲状态，即等待 64 个 range 线程将该分片消费结束。

缓冲区中前 8 个位置是可替换的，后 8 个位置是不可替换的，只能写入一次，这样第二次 range 访问到这 8 个缓冲区时就不需要读磁盘。

```C++
void readDisk() {
    int cacheIndex;
    char *cache;

    while (true) {
        //互斥锁，多线程竞争写缓冲区位置
        readDiskLogIdMtx.lock();
		//待读取分片和缓冲区位置
        int logId = readDiskLogId++;
        cacheIndex = logId % ACTIVE_CACHE_NUM;

        //等待缓冲区空闲
        if (!isCacheWritable[cacheIndex]) {
            rangeCacheFinish[cacheIndex].lock();
            while (!isCacheWritable[cacheIndex]) {
                rangeCacheFinish[cacheIndex].wait();
            }
            rangeCacheFinish[cacheIndex].unlock();
        }
        isCacheWritable[cacheIndex] = false;
        currentCacheLogId[cacheIndex] = logId;
        cache = valueCache + cacheIndex * CACHE_SIZE;
        readDiskLogIdMtx.unlock();
        keyValueLogs[logId]->readValue(0, cache, (size_t) keyValueLogs[logId]->size());
        
        //通知 range 线程缓冲区可读
        readDiskFinish[cacheIndex].lock();
        isCacheReadable[cacheIndex] = true;
        readDiskFinish[cacheIndex].notify_all();
        readDiskFinish[cacheIndex].unlock();

    }
}
```

#### 3.5 全局参数

```c++
//分区数量
const int LOG_NUM = 4096;
//分区容量
const int NUM_PER_SLOT = 1024 * 16;
//value 文件大小 = 64M
const size_t VALUE_LOG_SIZE = NUM_PER_SLOT << 12;
//key 文件大小
const size_t KEY_LOG_SIZE = NUM_PER_SLOT << 3;
//文件数
const int FILE_NUM = 64;
//索引数组大小
const int SORT_LOG_SIZE = NUM_PER_SLOT;
//缓冲区单元大小 = 64M
const size_t CACHE_SIZE = VALUE_LOG_SIZE;
//缓冲区容量
const int CACHE_NUM = 16;
//可替换缓冲区单元
const int ACTIVE_CACHE_NUM = 8;
//保留缓冲区单元
const int RESERVE_CACHE_NUM = CACHE_NUM - ACTIVE_CACHE_NUM;
//预读缓冲区单元
const int PREPARE_CACHE_NUM = 4;
//value cache 容量
const int PAGE_PER_BLOCK = 4;
//并发恢复线程数
const int RECOVER_THREAD = 64;
//读磁盘线程数
const int READDISK_THREAD = 2;
```

### 4 经验总结和感想

#### 文件读写方式

这次比赛开始利用 Java 进行开发，使用 NIO 中 FileChannel 配合 HeapByteBuffer 进行 value 读写，MappedByteBuffer 进行 key 文件的读写。根据源码发现，FileChannel 每次读写都要先从堆内内存 copy 到堆外内存，这就有了一次损耗，然后我们直接改用 DirectByteBuffer，明显有所好转。虽然用了堆外内存，但是实际上文件读写仍然需要经过内核 page cache 缓冲。这其中又多了一次拷贝，并且评测程序需要计算 drop cache 的开销。

Java 原生并不支持直接 I/O，于是我们使用 JNA 直接调用系统函数实现了 Direct I/O，读写性能有明显提升。最后我们改用 C++ 开发，直接使用 Linux 提供的 Direct I/O 文件读写模式，获得了很好的效果。

#### Java 对比 C++

对于这次比赛的单机数据库引擎， Java 相比 C++ 存在一些劣势：

- Java 使用操作系统原生函数不够方便，比如，C++ 可以直接使用 Linux 提供的 Direct I/O 模式，而原生 JDK 不支持直接 I/O；Java 对 mmap 的回收也比较麻烦。
- C++ 相比与 Java 而言内存消耗较少，可以自己控制内存的释放，节省了 GC 时间。
- Java 不够贴近系统底层，需要通过 JVM 解释字节码执行，相比于 C++ 直接编译后执行会稍慢一些。

#### 绑核减少线程切换

赛后与其他选手交流，发现很多人设置了 CPU 亲和性（Affinity）调度属性，将读磁盘线程与 CPU 逻辑核心绑定，range 阶段由此快 1~2 s，并且能保持测评的稳定性。

因为 range 阶段的瓶颈在读磁盘线程的速度，在使用条件变量时，I/O 线程可能被阻塞（wait），线程被唤醒后进入运行队列排队，这个过程使得 I/O 停顿，从而影响整体的性能。

可以考虑使用 `while(true)` 轮询的自旋方式代替条件变量的阻塞方式，或者直接将 I/O 线程与 CPU 核心绑定以减少 I/O 线程切换带来的时间损失。

#### 对 kv 数据库的思考

在比赛的一开始，我们对一些常用的 kv 数据库进行了调研，并且阅读了部分代码，想找到一些可以借鉴的地方。经过调研，发现主要使用的储存结构有如下几种：

- Hash：Redis、Memcache
- B+ Tree：MySQL-Innodb、LMDB
- Log-Structured Merge Tree: RocksDB、LevelDB

我们原本的想法是，第一版先用 Hash 做，尽快实现出来，后面再尝试其他的数据结构，但是在实现的过程中，逐渐抛弃了使用 B+ Tree、LSM-Tree 的想法，因为我们发现，LevelDB、LMDB 等 KV 数据库产品，面向的一定是通用的场景，不会有苛刻内存方面的限制，而且提供的功能也远远要多于比赛中所要求的，因此他们所采用的数据结构一定是性能和功能权衡后的结果，而针对比赛，还需要使用更有针对性的结构。

本次比赛中的场景有这样一些特征：单机、内存受限、数据类型单一、数据定长、以及数据分布完全随机。针对这些特征，我们才最终决定使用数组+排序+二分查找作为数据的索引结构，结果也证明了，这样简单的结构效果反而比 KV 数据库中常用的结构效果好。















