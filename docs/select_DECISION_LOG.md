# SELECT阶段决策日志
在实际开发中遇到的具体问题，与单纯选型决策不同，我会用场景-方案-权衡-决策四步展开

## 3月23日：关于缓冲区的具体实现

场景：仅100并发下，P50-P90均稳定至15ms以内，但是P99抖动剧烈，范围在10ms-1000ms，100并发后，P50-P99仍然稳定在15ms以内，P99不再抖动，达到稳定值1000ms。推测是调度不均导致，我这里选择先**限制**单次处理所有活跃连接的**请求量**。要想设置单个连接的请求量，要给每个连接都给一个自己的缓冲区。

我需要一个简单的方案快速验证是单次处理所有活跃连接的**请求量**过多，导致单次处理时间太长，少量连接长时间等待，验证后，再去根据流量网关场景进行设计。

提前考虑：要给每个连接维护缓冲区的话，缓冲区就属于socket类的成员了，我现在只有一个`char [] buffer 1024`，那我得把现在的固定大小通用临时缓冲区删掉，因为内核缓冲区→拷贝→临时缓冲区→拷贝→连接缓冲区，有两次拷贝，损耗太大。

方案：工业界通用点————给每个连接单独维护缓冲区

|  常见库  | 数据结构   | 零拷贝         | 可扩展       | 复杂度 |
|----------|------------|----------------|--------------|--------|
| Muduo    | vector     | 否（memmove）  | 是           | 中     |
| Asio     | 用户提供   | 是             | 取决于用户   | 高     |
| Netty    | 环形+池化  | 部分（视图）   | 是           | 高     |
| libevent | 链表       | 是             | 是           | 高     |

trade-off:
1. asio的显然不太适合我，我需要做来学习，不能让用户实现
2. libevent的链表比较适合大文件场景，在我的场景复杂度成本太高
3. netty是因为Java有GC机制，频繁分配内存会导致GC停顿，最好实现零拷贝，避免内存分配和memmove
4. muduo的复杂度最低，虽然memmove是O(n),但CPU花费不大，足够大量场景的服务器使用。我或许可以先用muduo的vector方案作为快速验证，后续再根据我的流量网关场景进行设计。
5. 但是muduo复杂度只是相较这几个里面比较低，既然后续还要根据自己场景特性去设计，还是采取一个复杂度更低的方案吧。

最终决策: 自己用string实现一个read_buffer(先不做write_buffer)，不设计成buffer类，而是作为socket的成员变量。

决策反馈: 实现过程发现不设计成buffer类不行，还是把buffer相关的方法封装起来并且创建一个类更合理，以及还需要一个Connection类才能来实现一个连接一个缓冲区，否则会导致buffer共享、P99随机崩塌。所以有了目前的样子，之后还会进一步改进和优化，也会添加更多方法

## 4月10日：关于架构设计，谁来维护map<int,Connection>connections

场景: TCPServer类职责过重，承担着管理fd、connection、回调函数等等多项服务，从用户视角该库为一个黑盒，于是进行多重拆解，目标为架空TCPServer类，使得底层组件没有TCPServer也完全可以由组装来运行，形成依赖倒置。先后将Socket替换为SocketListener，创建SelectPoller封装fd与conn的添加/移除并维护vector<int> client_fds，问题是map<int,Connection>connections仍然由TCPServer连接。

方案：
1. 让TCPServer或者eventLoop维护连接map
优点: 改动小，复杂度低
缺点: 不符合TCPServer架空的目标，灵活性较低
2. 让SelectPoller维护连接map
优点: 改动小，复杂度低，而且在removeFd()中也实现remove Client_fd就可以确保IO模型Fd与client_fd的vector的一致性，防止用户忘记额外调用removeClient_fd
缺点: 又导致SelectPoller职责过重，一个类既要封装IO逻辑管理系统调用的FD，又要维护client_fds的vector，又要维护map连接
3. 创建新类ConnectionManager维护map
优点: 1. 降低耦合度，彻底避免了上帝类的存在，让各类作为组件存在可以组合使用。2. 符合职责单一原则，让SelectPoller只承担管理IO模型的单一职责，ConnectionManager只承担管理连接、client_fd的职责
缺点: 1. 中等复杂度，从vector添加/移除client_fd两三行的代码需要从SelectPoller类提炼出来，有过度设计的风险。2. 需要保证IO模型Fd与client_fd的vector的一致性，易出错

trade-off: 
我们的定位是一个可组装、灵活性优先的 C++ 网络工具箱，而非开箱即用的框架——灵活性的权重高于实现复杂度。为了灵活性、职责单一、解耦度可以承担一定过度设计的风险。对于保证IO模型Fd与client_fd的vector的一致性的问题，可以暴露addClient和removeClient，比Fd更加符合用户的语义直觉，然后在内部封装添加/移除client_fd并调用SelectPoller类的addFd与removeFd，既确保了语义正确又确保了Fd维护一致性

最终决策: 创建ConnectionManager维护map。一致性问题用这个方案解决，内部封装添加/移除client_fd并调用SelectPoller类的addFd与removeFd

决策反馈: 可以创建ConnectionManager维护map，但是一致性问题不能用这个方案解决，因为维护max_fd是需要遍历client_fds的，client_fds只能暂时属于SelectPoller，等日后替换为epoll模型这个问题得以被解决，再将client_fds这个vector归还给ConnectionManager类


